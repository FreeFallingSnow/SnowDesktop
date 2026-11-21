#include "widget_view_tree.h"
#include "widget_view_geometry.h"
#include "widget_view_layout.h"
#include "widget_view_layout_animation.h"
#include "widget_view_node_rules.h"
#include "widget_view_validation.h"
#include "widget/api/widget_view_contract.h"
#include "widget/runtime/widget_logical_slot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
using detail::ApplyTransform;
using detail::FiniteInRange;
using detail::HasCollectionPlaceholder;
using detail::IntersectRects;
using detail::IsCollectionContainer;
using detail::IsInputNode;
using detail::IsScrollContainer;
using detail::IsVirtualCollection;
using detail::MaximumDimension;
using detail::MaximumScrollExtent;
using detail::VirtualCollectionMainGap;

struct CivilDate
{
    int year = 0;
    int month = 0;
    int day = 0;
    int weekday = 0;
};

int DaysInMonth(int year, int month) noexcept
{
    static constexpr std::array<int, 12> days = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12) return 0;
    if (month != 2) return days[static_cast<std::size_t>(month - 1)];
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
        ? 29 : 28;
}

long long DaysFromCivil(int year, unsigned month, unsigned day) noexcept
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097 + doe - 719468;
}

CivilDate CivilFromDays(long long days) noexcept
{
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const int month = static_cast<int>(mp) + (mp < 10 ? 3 : -9);
    year += month <= 2;
    int weekday = static_cast<int>((days - 719468 + 4) % 7);
    if (weekday < 0) weekday += 7;
    return { year, month, static_cast<int>(day), weekday + 1 };
}

bool ParseCivilDate(std::string_view value, CivilDate& date) noexcept
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        return false;
    int parts[3]{};
    const std::array<std::pair<std::size_t, std::size_t>, 3> ranges = {
        std::pair<std::size_t, std::size_t>{ 0, 4 }, { 5, 2 }, { 8, 2 }
    };
    for (std::size_t part = 0; part < ranges.size(); ++part)
    {
        const auto [offset, length] = ranges[part];
        for (std::size_t index = 0; index < length; ++index)
        {
            const char ch = value[offset + index];
            if (ch < '0' || ch > '9') return false;
            parts[part] = parts[part] * 10 + (ch - '0');
        }
    }
    if (parts[0] < 1 || parts[0] > 9999 || parts[1] < 1 ||
        parts[1] > 12 || parts[2] < 1 ||
        parts[2] > DaysInMonth(parts[0], parts[1]))
        return false;
    const long long serial = DaysFromCivil(parts[0],
        static_cast<unsigned>(parts[1]), static_cast<unsigned>(parts[2]));
    int weekday = static_cast<int>((serial + 4) % 7);
    if (weekday < 0) weekday += 7;
    date = { parts[0], parts[1], parts[2], weekday + 1 };
    return true;
}

std::string FormatCivilDate(const CivilDate& date)
{
    char buffer[11]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
        date.year, date.month, date.day);
    return buffer;
}

void ApplyCollectionSelectionState(ViewNode& node)
{
    if (IsCollectionContainer(node.type) &&
        node.collectionContent == ViewCollectionContent::Items &&
        node.selectionMode != ViewSelectionMode::None)
    {
        const auto change = node.events.find("change");
        for (auto& child : node.children)
        {
            child.inheritedSelectionMode = node.selectionMode;
            child.inheritedSelectedKeys = node.selectedKeys;
            child.selected = std::find(node.selectedKeys.begin(),
                node.selectedKeys.end(), child.key) !=
                node.selectedKeys.end();
            if (change != node.events.end())
                child.inheritedSelectionChangeAction = change->second;
        }
    }
    for (auto& child : node.children)
        ApplyCollectionSelectionState(child);
}

void ClearCollectionSelectionState(ViewNode& node)
{
    if (node.inheritedSelectionMode != ViewSelectionMode::None)
        node.selected = false;
    node.inheritedSelectionMode = ViewSelectionMode::None;
    node.inheritedSelectedKeys.clear();
    node.inheritedSelectionChangeAction.reset();
    for (auto& child : node.children)
        ClearCollectionSelectionState(child);
}

void TranslateTree(ViewNode& node, float deltaX, float deltaY) noexcept
{
    node.frame.x += deltaX;
    node.frame.y += deltaY;
    if (node.clipFrame)
    {
        node.clipFrame->x += deltaX;
        node.clipFrame->y += deltaY;
    }
    for (auto& child : node.children)
        TranslateTree(child, deltaX, deltaY);
}

void ApplyStickyHeaders(ViewNode& node, const ViewRect& viewport) noexcept
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden ||
        IsScrollContainer(node.type))
        return;
    if (node.type == ViewNodeType::List &&
        node.orientation == ViewOrientation::Vertical)
    {
        std::vector<ViewNode*> headers;
        for (auto& child : node.children)
        {
            if (child.visible &&
                child.visibility != ViewVisibility::Hidden && child.sticky)
                headers.push_back(&child);
        }
        const ViewRect content = ViewNodeContentRect(node);
        const float listBottom = content.y + content.height;
        for (std::size_t index = 0; index < headers.size(); ++index)
        {
            ViewNode& header = *headers[index];
            const float originalY = header.frame.y;
            float presentedY = std::max(originalY, viewport.y);
            presentedY = std::min(presentedY,
                listBottom - header.frame.height);
            if (index + 1 < headers.size())
                presentedY = std::min(presentedY,
                    headers[index + 1]->frame.y - header.frame.height);
            if (presentedY > originalY + 0.001f)
            {
                TranslateTree(header, 0.0f, presentedY - originalY);
                header.stickyPresented = true;
            }
        }
    }
    for (auto& child : node.children)
        ApplyStickyHeaders(child, viewport);
}

void ApplyVirtualStickyHeaders(ViewNode& node,
    const ViewRect& viewport) noexcept
{
    if (node.type != ViewNodeType::VirtualList ||
        node.sectionHeaderIndices.empty() ||
        node.virtualChildIndices.size() != node.children.size())
        return;
    std::vector<ViewNode*> headers;
    headers.reserve(node.sectionHeaderIndices.size());
    for (std::size_t childIndex = 0;
        childIndex < node.children.size(); ++childIndex)
    {
        ViewNode& child = node.children[childIndex];
        if (child.visible && child.visibility != ViewVisibility::Hidden &&
            std::binary_search(node.sectionHeaderIndices.begin(),
                node.sectionHeaderIndices.end(),
                node.virtualChildIndices[childIndex]))
            headers.push_back(&child);
    }
    const float listBottom = viewport.y +
        node.scrollContentExtent - node.scrollOffset;
    for (std::size_t index = 0; index < headers.size(); ++index)
    {
        ViewNode& header = *headers[index];
        const float originalY = header.frame.y;
        float presentedY = std::max(originalY, viewport.y);
        presentedY = std::min(presentedY,
            listBottom - header.frame.height);
        if (index + 1 < headers.size())
            presentedY = std::min(presentedY,
                headers[index + 1]->frame.y - header.frame.height);
        if (presentedY > originalY + 0.001f)
        {
            TranslateTree(header, 0.0f, presentedY - originalY);
            header.stickyPresented = true;
        }
    }
}

const ViewNode* FindVisibleDescendantByKey(const ViewNode& node,
    std::string_view key) noexcept
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden)
        return nullptr;
    if (node.key == key) return &node;
    for (const auto& child : node.children)
    {
        if (const auto* found = FindVisibleDescendantByKey(child, key))
            return found;
    }
    return nullptr;
}

bool ApplyScrollState(ViewNode& node,
    const ViewScrollOffsetResolver& resolver,
    std::vector<ViewScrollViewport>& viewports,
    const std::optional<ViewRect>& inheritedClip,
    float surfaceHeight, std::size_t& scrollContainers,
    std::string& error)
{
    if (!node.visible || node.visibility == ViewVisibility::Hidden)
        return true;
    std::optional<ViewRect> childClip = inheritedClip;
    if (!IsScrollContainer(node.type) && node.clipFrame)
    {
        childClip = IntersectRects(inheritedClip, *node.clipFrame);
    }
    if (node.type == ViewNodeType::Select && node.expanded)
    {
        const ViewRect popup = ViewSelectPopupFrame(node, surfaceHeight);
        const float optionHeight = std::max(28.0f,
            std::min(48.0f, node.fontSize * 1.8f));
        const float contentExtent = optionHeight *
            static_cast<float>(node.options.size());
        const float maximum = std::max(0.0f,
            contentExtent - popup.height);
        const std::optional<float> resolved = resolver
            ? resolver(node.key, maximum) : std::optional<float>{};
        float requested = resolved.value_or(0.0f);
        if (!resolved)
        {
            const auto selected = std::find_if(node.options.begin(),
                node.options.end(), [&node](const auto& option) {
                    return option.value == node.selectedValue;
                });
            if (selected != node.options.end())
                requested = optionHeight * static_cast<float>(
                    std::distance(node.options.begin(), selected) + 1) -
                    popup.height;
        }
        if (!std::isfinite(requested))
        {
            error = "view select scroll resolver returned a non-finite offset";
            return false;
        }
        node.scrollOffset = std::clamp(requested, 0.0f, maximum);
        node.scrollViewportExtent = popup.height;
        node.scrollContentExtent = contentExtent;
        viewports.push_back({ node.key, popup,
            ViewOrientation::Vertical, popup.height, contentExtent,
            node.scrollOffset, maximum, !resolved.has_value(), true });
    }
    if (IsScrollContainer(node.type))
    {
        if (++scrollContainers > ViewTreeLimits::MaximumScrollContainers)
        {
            error = "view scroll container limit exceeded (32)";
            return false;
        }
        if (node.type == ViewNodeType::Scroll &&
            (node.children.size() != 1 || !node.children.front().visible))
        {
            error = "view scroll state requires one visible child";
            return false;
        }
        const ViewRect clip = ViewNodeContentRect(node);
        if (clip.width <= 0.0f || clip.height <= 0.0f)
        {
            error = "view scroll viewport must have positive content bounds";
            return false;
        }
        node.clipFrame = clip;
        const bool vertical = node.orientation == ViewOrientation::Vertical;
        const float viewportExtent = vertical ? clip.height : clip.width;
        float contentExtent = viewportExtent;
        float maximum = 0.0f;
        ViewVirtualRange virtualRange;
        if (IsVirtualCollection(node.type) &&
            !HasCollectionPlaceholder(node))
        {
            if (node.estimatedItemSize)
            {
                if (!ComputeViewVariableVirtualRange(node.itemCount,
                        *node.estimatedItemSize,
                        VirtualCollectionMainGap(node), viewportExtent,
                        0.0f, node.overscan, node.virtualMeasurements,
                        virtualRange, error))
                    return false;
            }
            else if (!ComputeViewVirtualRange(node.itemCount,
                    node.itemExtent,
                    node.type == ViewNodeType::VirtualGrid
                        ? node.columns : 1,
                    VirtualCollectionMainGap(node), viewportExtent,
                    0.0f, node.overscan, virtualRange, error))
                return false;
            contentExtent = virtualRange.contentExtent;
            maximum = virtualRange.maximum;
        }
        else
        {
            const ViewNode& child = node.children.front();
            contentExtent = std::max(viewportExtent,
                vertical ? child.frame.y + child.frame.height +
                        child.margin.bottom - clip.y :
                    child.frame.x + child.frame.width +
                        child.margin.right - clip.x);
            if (!FiniteInRange(contentExtent, 0.0f, MaximumScrollExtent))
            {
                error = "view scroll content extent exceeds 1000000";
                return false;
            }
            maximum = std::max(0.0f,
                contentExtent - viewportExtent);
        }
        const std::optional<float> resolved = resolver
            ? resolver(node.key, maximum) : std::optional<float>{};
        float requested = resolved.value_or(0.0f);
        bool initialized = !resolved.has_value();
        if (initialized && IsVirtualCollection(node.type) &&
            HasCollectionPlaceholder(node) && node.initialScrollIndex)
            initialized = false;
        if (initialized && node.type == ViewNodeType::Scroll &&
            node.initialScrollKey)
        {
            const ViewNode* target = FindVisibleDescendantByKey(
                node.children.front(), *node.initialScrollKey);
            if (!target)
            {
                error = "view initialScrollKey does not reference a visible descendant";
                return false;
            }
            const float targetStart = vertical
                ? target->frame.y - clip.y : target->frame.x - clip.x;
            const float targetEnd = targetStart +
                (vertical ? target->frame.height : target->frame.width);
            if (targetStart < 0.0f) requested = targetStart;
            else if (targetEnd > viewportExtent)
                requested = targetEnd - viewportExtent;
        }
        else if (initialized && IsVirtualCollection(node.type) &&
            !HasCollectionPlaceholder(node) && node.initialScrollIndex)
        {
            if (node.estimatedItemSize)
            {
                if (!ComputeViewVariableVirtualItemScrollOffset(
                        node.itemCount, *node.estimatedItemSize,
                        VirtualCollectionMainGap(node), viewportExtent,
                        0.0f, *node.initialScrollIndex, "nearest",
                        node.virtualMeasurements, requested, error))
                    return false;
            }
            else if (!ComputeViewVirtualItemScrollOffset(node.itemCount,
                    node.itemExtent,
                    node.type == ViewNodeType::VirtualGrid
                        ? node.columns : 1,
                    VirtualCollectionMainGap(node), viewportExtent,
                    0.0f, *node.initialScrollIndex, "nearest",
                    requested, error))
                return false;
        }
        if (!std::isfinite(requested))
        {
            error = "view scroll resolver returned a non-finite offset";
            return false;
        }
        const float offset = std::clamp(requested, 0.0f, maximum);
        node.scrollOffset = offset;
        node.scrollViewportExtent = viewportExtent;
        node.scrollContentExtent = contentExtent;
        if (IsVirtualCollection(node.type) &&
            !HasCollectionPlaceholder(node))
        {
            ViewVirtualRange visibleRange;
            if (node.estimatedItemSize)
            {
                if (!ComputeViewVariableVirtualRange(node.itemCount,
                        *node.estimatedItemSize,
                        VirtualCollectionMainGap(node), viewportExtent,
                        offset, 0, node.virtualMeasurements,
                        visibleRange, error))
                    return false;
            }
            else if (!ComputeViewVirtualRange(node.itemCount, node.itemExtent,
                    node.type == ViewNodeType::VirtualGrid
                        ? node.columns : 1,
                    VirtualCollectionMainGap(node), viewportExtent,
                    offset, 0, visibleRange, error))
                return false;
            if (node.itemCount > 0)
            {
                std::optional<std::size_t> expectedStickyHeader;
                const auto nextHeader = std::upper_bound(
                    node.sectionHeaderIndices.begin(),
                    node.sectionHeaderIndices.end(),
                    visibleRange.firstIndex);
                if (nextHeader != node.sectionHeaderIndices.begin())
                    expectedStickyHeader = *std::prev(nextHeader);
                if (node.stickyHeaderIndex != expectedStickyHeader)
                {
                    error = "virtualList stickyHeaderIndex does not match the current visible range";
                    return false;
                }
                const std::size_t windowChildOffset =
                    node.stickyHeaderIndex &&
                    *node.stickyHeaderIndex < node.firstIndex ? 1 : 0;
                const std::size_t windowChildCount =
                    node.children.size() - windowChildOffset;
                const std::size_t providedLast = node.firstIndex +
                    windowChildCount - 1;
                if (node.firstIndex > visibleRange.firstIndex ||
                    providedLast < visibleRange.lastIndex)
                {
                    error = "virtual collection window does not cover the visible range";
                    return false;
                }
            }
            for (auto& child : node.children)
                TranslateTree(child,
                    vertical ? 0.0f : -offset,
                    vertical ? -offset : 0.0f);
            if (vertical) ApplyVirtualStickyHeaders(node, clip);
        }
        else if (!IsVirtualCollection(node.type))
        {
            TranslateTree(node.children.front(),
                vertical ? 0.0f : -offset,
                vertical ? -offset : 0.0f);
            if (vertical)
                ApplyStickyHeaders(node.children.front(), clip);
        }

        childClip = IntersectRects(inheritedClip, clip);
        const ViewRect visibleFrame = childClip.value_or(clip);
        viewports.push_back({ node.key, visibleFrame, node.orientation,
            viewportExtent, contentExtent, offset, maximum, initialized });
    }
    for (auto& child : node.children)
        if (!ApplyScrollState(child, resolver, viewports, childClip,
                surfaceHeight, scrollContainers, error)) return false;
    return true;
}

ViewResolvedTransform ComposeTransform(
    const ViewResolvedTransform& local,
    const ViewResolvedTransform& parent) noexcept
{
    return {
        local.m11 * parent.m11 + local.m12 * parent.m21,
        local.m11 * parent.m12 + local.m12 * parent.m22,
        local.m21 * parent.m11 + local.m22 * parent.m21,
        local.m21 * parent.m12 + local.m22 * parent.m22,
        local.dx * parent.m11 + local.dy * parent.m21 + parent.dx,
        local.dx * parent.m12 + local.dy * parent.m22 + parent.dy,
    };
}

ViewResolvedTransform LocalTransform(const ViewRect& frame,
    const std::optional<ViewTransform>& transform) noexcept
{
    if (!transform) return {};
    const auto& value = *transform;
    const float originX = frame.x + frame.width * value.originX;
    const float originY = frame.y + frame.height * value.originY;
    const float radians = value.rotate *
        (3.14159265358979323846f / 180.0f);
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const float skewX = std::tan(value.skewX *
        (3.14159265358979323846f / 180.0f));
    const float skewY = std::tan(value.skewY *
        (3.14159265358979323846f / 180.0f));
    const float scaleX = value.scale * value.scaleX;
    const float scaleY = value.scale * value.scaleY;
    const ViewResolvedTransform result{
        scaleX * (cosine - skewY * sine),
        scaleX * (sine + skewY * cosine),
        scaleY * (skewX * cosine - sine),
        scaleY * (skewX * sine + cosine),
        0.0f,
        0.0f,
    };
    return { result.m11, result.m12, result.m21, result.m22,
        originX - originX * result.m11 - originY * result.m21 +
            value.translateX,
        originY - originX * result.m12 - originY * result.m22 +
            value.translateY };
}

ViewResolvedTransform LocalTransform(const ViewNode& node) noexcept
{
    return LocalTransform(node.frame, node.transform);
}

bool IsPositiveAxisAlignedTransform(
    const ViewResolvedTransform& transform) noexcept
{
    constexpr float epsilon = 0.00001f;
    return transform.m11 > epsilon && transform.m22 > epsilon &&
        std::abs(transform.m12) <= epsilon &&
        std::abs(transform.m21) <= epsilon;
}

bool IsAxisAlignedTransform(const ViewResolvedTransform& transform) noexcept
{
    constexpr float epsilon = 0.00001f;
    return (std::abs(transform.m12) <= epsilon &&
            std::abs(transform.m21) <= epsilon) ||
        (std::abs(transform.m11) <= epsilon &&
            std::abs(transform.m22) <= epsilon);
}

bool SingularValues(const ViewResolvedTransform& transform,
    float& minimum, float& maximum) noexcept
{
    const double squaredTrace =
        static_cast<double>(transform.m11) * transform.m11 +
        static_cast<double>(transform.m12) * transform.m12 +
        static_cast<double>(transform.m21) * transform.m21 +
        static_cast<double>(transform.m22) * transform.m22;
    const double determinant =
        static_cast<double>(transform.m11) * transform.m22 -
        static_cast<double>(transform.m12) * transform.m21;
    const double discriminant = std::sqrt(std::max(
        0.0, squaredTrace * squaredTrace -
            4.0 * determinant * determinant));
    const double maximumSquared = (squaredTrace + discriminant) * 0.5;
    if (!std::isfinite(maximumSquared) || maximumSquared <= 0.0)
        return false;
    maximum = static_cast<float>(std::sqrt(maximumSquared));
    minimum = static_cast<float>(std::abs(determinant) / maximum);
    return std::isfinite(minimum) && std::isfinite(maximum);
}

struct TransformMatch
{
    ViewResolvedTransform node;
    std::optional<ViewRect> parentClip;
    std::optional<ViewRect> nodeClip;
    std::size_t keyLength = 0;
    bool found = false;
};

std::optional<ViewRect> TransformExitClip(
    const std::optional<ViewRect>& clip,
    const ViewResolvedTransform& transform) noexcept
{
    return clip ? std::optional<ViewRect>(ApplyViewTransform(*clip, transform))
                : std::nullopt;
}

std::optional<ViewRect> IntersectExitClips(
    const std::optional<ViewRect>& inherited,
    const std::optional<ViewRect>& clip) noexcept
{
    // An absent clip is unbounded. An empty rectangle clips everything.
    if (!clip) return inherited;
    return IntersectRects(inherited, *clip).value_or(ViewRect{});
}

void FindTransform(const ViewNode& node, std::string_view key,
    const ViewResolvedTransform& parent,
    const std::optional<ViewRect>& inheritedClip,
    bool inheritedClipActive,
    TransformMatch& match) noexcept
{
    const ViewResolvedTransform current =
        ComposeTransform(LocalTransform(node), parent);
    std::optional<ViewRect> childClip = inheritedClip;
    bool childClipActive = inheritedClipActive;
    if (node.clipFrame)
    {
        const ViewRect transformedClip =
            ApplyViewTransform(*node.clipFrame, current);
        childClip = inheritedClipActive
            ? (inheritedClip
                ? IntersectRects(inheritedClip, transformedClip)
                : std::nullopt)
            : std::optional<ViewRect>(transformedClip);
        childClipActive = true;
    }
    const bool exact = key == node.key;
    const bool generated = key.size() > node.key.size() &&
        key.starts_with(node.key) && key[node.key.size()] == '/';
    if ((exact || generated) && node.key.size() >= match.keyLength)
    {
        match.node = current;
        match.parentClip = inheritedClipActive
            ? std::optional<ViewRect>(
                inheritedClip.value_or(ViewRect{}))
            : std::nullopt;
        match.nodeClip = childClipActive
            ? std::optional<ViewRect>(childClip.value_or(ViewRect{}))
            : std::nullopt;
        match.keyLength = node.key.size();
        match.found = true;
    }
    for (const auto& child : node.children)
        FindTransform(child, key, current, childClip,
            childClipActive, match);
}

enum class ViewPresencePhase
{
    Target,
    Enter,
    Exit,
};

bool ValidateTransforms(const ViewNode& node,
    const ViewResolvedTransform& parent, std::string& error,
    ViewPresencePhase phase = ViewPresencePhase::Target)
{
    const ViewPresenceTransition* presence = nullptr;
    if (phase == ViewPresencePhase::Enter && node.enterTransition)
        presence = &*node.enterTransition;
    else if (phase == ViewPresencePhase::Exit && node.exitTransition)
        presence = &*node.exitTransition;
    const std::optional<ViewTransform>& transform =
        presence && presence->transform
        ? presence->transform : node.transform;
    if (transform)
    {
        const auto& value = *transform;
        if (!FiniteInRange(value.translateX,
                -MaximumDimension, MaximumDimension) ||
            !FiniteInRange(value.translateY,
                -MaximumDimension, MaximumDimension) ||
            !FiniteInRange(value.scale, 0.05f, 8.0f) ||
            !FiniteInRange(value.scaleX, 0.05f, 8.0f) ||
            !FiniteInRange(value.scaleY, 0.05f, 8.0f) ||
            !FiniteInRange(value.scale * value.scaleX, 0.05f, 8.0f) ||
            !FiniteInRange(value.scale * value.scaleY, 0.05f, 8.0f) ||
            !FiniteInRange(value.rotate, -360.0f, 360.0f) ||
            !FiniteInRange(value.skewX, -80.0f, 80.0f) ||
            !FiniteInRange(value.skewY, -80.0f, 80.0f) ||
            !FiniteInRange(value.originX, 0.0f, 1.0f) ||
            !FiniteInRange(value.originY, 0.0f, 1.0f))
        {
            error = phase == ViewPresencePhase::Enter
                ? "view enterTransition transform fields are outside their limits"
                : (phase == ViewPresencePhase::Exit
                    ? "view exitTransition transform fields are outside their limits"
                    : "view transform fields are outside their limits");
            return false;
        }
    }
    const ViewResolvedTransform current =
        ComposeTransform(LocalTransform(node.frame, transform), parent);
    float minimumScale = 0.0f;
    float maximumScale = 0.0f;
    if (!SingularValues(current, minimumScale, maximumScale) ||
        !FiniteInRange(minimumScale, 1.0f / 64.0f, 64.0f) ||
        !FiniteInRange(maximumScale, 1.0f / 64.0f, 64.0f))
    {
        error = "view cumulative transform axes must remain between 1/64 and 64";
        return false;
    }
    const ViewRect transformed = ApplyViewTransform(node.frame, current);
    if (!FiniteInRange(transformed.x,
            -MaximumScrollExtent, MaximumScrollExtent) ||
        !FiniteInRange(transformed.y,
            -MaximumScrollExtent, MaximumScrollExtent) ||
        !FiniteInRange(transformed.width, 0.0f, MaximumScrollExtent) ||
        !FiniteInRange(transformed.height, 0.0f, MaximumScrollExtent))
    {
        error = "view transformed bounds exceed 1000000";
        return false;
    }
    if (node.clipFrame && !IsAxisAlignedTransform(current))
    {
        error = "view rotated clips are not supported";
        return false;
    }
    if ((IsInputNode(node.type) || IsScrollContainer(node.type) ||
            node.type == ViewNodeType::SlotSurface ||
            node.type == ViewNodeType::SlotItem) &&
        !IsPositiveAxisAlignedTransform(current))
    {
        error = "view host-managed controls and logical slots require an axis-aligned transform";
        return false;
    }
    for (const auto& child : node.children)
        if (!ValidateTransforms(child, current, error, phase))
            return false;
    return true;
}

bool HasTransitionProperty(const ViewTransition& transition,
    ViewTransitionProperty property) noexcept
{
    return std::find(transition.properties.begin(),
        transition.properties.end(), property) !=
        transition.properties.end();
}

std::uint32_t InterpolateTransitionColor(
    std::uint32_t start, std::uint32_t target, float progress) noexcept
{
    const auto channel = [progress](std::uint32_t from,
        std::uint32_t to) {
        return static_cast<std::uint32_t>(std::lround(
            static_cast<float>(from) +
            (static_cast<float>(to) - static_cast<float>(from)) *
                progress));
    };
    return (channel((start >> 16) & 0xFF, (target >> 16) & 0xFF) << 16) |
        (channel((start >> 8) & 0xFF, (target >> 8) & 0xFF) << 8) |
        channel(start & 0xFF, target & 0xFF);
}

bool HasTransitionDifference(const ViewTransitionPresentation& start,
    const ViewTransitionPresentation& target,
    const ViewTransition& transition) noexcept
{
    const auto changedColor = [](const auto& first, const auto& second) {
        return first && second && first != second;
    };
    return (HasTransitionProperty(transition,
                ViewTransitionProperty::Background) &&
            changedColor(start.style.background, target.style.background)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Foreground) &&
            changedColor(start.style.foreground, target.style.foreground)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::BorderColor) &&
            changedColor(start.style.borderColor,
                target.style.borderColor)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Opacity) &&
            start.style.opacity.value_or(1.0f) !=
                target.style.opacity.value_or(1.0f)) ||
        (HasTransitionProperty(transition,
                ViewTransitionProperty::Transform) &&
            start.transform.value_or(ViewTransform{}) !=
                target.transform.value_or(ViewTransform{}));
}

float InterpolateTransitionAngle(float start, float target,
    float progress) noexcept
{
    float delta = std::fmod(target - start, 360.0f);
    if (delta > 180.0f) delta -= 360.0f;
    else if (delta < -180.0f) delta += 360.0f;
    return start + delta * progress;
}

ViewTransform InterpolateTransitionTransform(const ViewTransform& start,
    const ViewTransform& target, float progress) noexcept
{
    const auto interpolate = [progress](float from, float to) {
        return from + (to - from) * progress;
    };
    return {
        interpolate(start.translateX, target.translateX),
        interpolate(start.translateY, target.translateY),
        interpolate(start.scale, target.scale),
        interpolate(start.originX, target.originX),
        interpolate(start.originY, target.originY),
        interpolate(start.scaleX, target.scaleX),
        interpolate(start.scaleY, target.scaleY),
        InterpolateTransitionAngle(start.rotate, target.rotate, progress),
        interpolate(start.skewX, target.skewX),
        interpolate(start.skewY, target.skewY),
    };
}

ViewTransitionPresentation InterpolateTransitionPresentation(
    const ViewTransitionPresentation& start,
    const ViewTransitionPresentation& target,
    const ViewTransition& transition,
    float progress) noexcept
{
    ViewTransitionPresentation result = target;
    progress = detail::ResolveViewTransitionProgress(transition.easing, progress);
    const auto applyColor = [progress](const auto& from, const auto& to,
        auto& output) {
        if (from && to)
            output = InterpolateTransitionColor(*from, *to, progress);
    };
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Background))
        applyColor(start.style.background, target.style.background,
            result.style.background);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Foreground))
        applyColor(start.style.foreground, target.style.foreground,
            result.style.foreground);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::BorderColor))
        applyColor(start.style.borderColor, target.style.borderColor,
            result.style.borderColor);
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Opacity))
    {
        const float from = start.style.opacity.value_or(1.0f);
        const float to = target.style.opacity.value_or(1.0f);
        result.style.opacity = from + (to - from) * progress;
    }
    if (HasTransitionProperty(
            transition, ViewTransitionProperty::Transform))
    {
        result.transform = InterpolateTransitionTransform(
            start.transform.value_or(ViewTransform{}),
            target.transform.value_or(ViewTransform{}), progress);
    }
    return result;
}
}

ViewResolvedTransform ResolveViewTransformForKey(
    const ViewNode& root, std::string_view key) noexcept
{
    TransformMatch match;
    FindTransform(root, key, {}, std::nullopt, false, match);
    return match.found ? match.node : ViewResolvedTransform{};
}

ViewResolvedTransform ResolveViewLocalTransform(
    const ViewNode& node) noexcept
{
    return LocalTransform(node);
}

ViewResolvedTransform ResolveViewLocalTransform(const ViewRect& frame,
    const std::optional<ViewTransform>& transform) noexcept
{
    return LocalTransform(frame, transform);
}

std::optional<ViewRect> ResolveViewClipForKey(
    const ViewNode& root, std::string_view key,
    bool includeMatchedNode) noexcept
{
    TransformMatch match;
    FindTransform(root, key, {}, std::nullopt, false, match);
    if (!match.found) return std::nullopt;
    return includeMatchedNode ? match.nodeClip : match.parentClip;
}

ViewRect ApplyViewTransform(const ViewRect& rect,
    const ViewResolvedTransform& transform) noexcept
{
    const std::array points{
        ApplyTransform(rect.x, rect.y, transform),
        ApplyTransform(rect.x + rect.width, rect.y, transform),
        ApplyTransform(rect.x, rect.y + rect.height, transform),
        ApplyTransform(rect.x + rect.width,
            rect.y + rect.height, transform),
    };
    float left = points.front().x;
    float top = points.front().y;
    float right = points.front().x;
    float bottom = points.front().y;
    for (const auto& point : points)
    {
        left = std::min(left, point.x);
        top = std::min(top, point.y);
        right = std::max(right, point.x);
        bottom = std::max(bottom, point.y);
    }
    return { left, top, right - left, bottom - top };
}

std::uint32_t ResolveViewThemeColor(ViewThemeColorToken token,
    const ViewThemePalette& palette) noexcept
{
    switch (token)
    {
    case ViewThemeColorToken::WidgetBackground:
        return palette.widgetBackground;
    case ViewThemeColorToken::Surface:
        return palette.surface;
    case ViewThemeColorToken::SurfaceVariant:
        return palette.surfaceVariant;
    case ViewThemeColorToken::TextPrimary:
        return palette.textPrimary;
    case ViewThemeColorToken::TextSecondary:
        return palette.textSecondary;
    case ViewThemeColorToken::TextDisabled:
        return palette.textDisabled;
    case ViewThemeColorToken::Border:
        return palette.border;
    case ViewThemeColorToken::BorderStrong:
        return palette.borderStrong;
    case ViewThemeColorToken::SystemAccent:
        return palette.systemAccent;
    case ViewThemeColorToken::AccentText:
        return palette.accentText;
    case ViewThemeColorToken::Info:
        return palette.info;
    case ViewThemeColorToken::Success:
        return palette.success;
    case ViewThemeColorToken::Warning:
        return palette.warning;
    case ViewThemeColorToken::Error:
        return palette.error;
    }
    return palette.textPrimary;
}

std::optional<std::uint32_t> ResolveViewThemeColor(
    const std::optional<std::uint32_t>& literal,
    const std::optional<ViewThemeColorToken>& token,
    const ViewThemePalette& palette) noexcept
{
    if (token) return ResolveViewThemeColor(*token, palette);
    return literal;
}

ViewStyle ResolveViewThemeStyle(const ViewStyle& style,
    const ViewThemePalette& palette) noexcept
{
    ViewStyle result = style;
    result.background = ResolveViewThemeColor(
        style.background, style.backgroundToken, palette);
    result.foreground = ResolveViewThemeColor(
        style.foreground, style.foregroundToken, palette);
    result.borderColor = ResolveViewThemeColor(
        style.borderColor, style.borderColorToken, palette);
    result.backgroundToken.reset();
    result.foregroundToken.reset();
    result.borderColorToken.reset();
    return result;
}

ViewTransitionRuntime::ViewTransitionRuntime() = default;
ViewTransitionRuntime::~ViewTransitionRuntime() = default;
ViewTransitionRuntime::ViewTransitionRuntime(ViewTransitionRuntime&&) noexcept = default;
ViewTransitionRuntime& ViewTransitionRuntime::operator=(
    ViewTransitionRuntime&&) noexcept = default;

void ViewTransitionRuntime::SetDurationScale(double scale) noexcept
{
    const double normalized = std::isfinite(scale) ? std::clamp(scale, 0.1, 10.0) : 1.0;
    if (durationScale_ == normalized) return;
    Settle();
    durationScale_ = normalized;
}

std::uint32_t ViewTransitionRuntime::ScaledDuration(std::uint32_t duration) const noexcept
{
    return detail::ScaleViewTransitionDuration(duration, durationScale_);
}

void ViewTransitionRuntime::Settle() noexcept
{
    exits_.clear();
    layoutAnimation_.reset();
    for (auto& [key, entry] : entries_)
    {
        (void)key;
        entry.active = false;
        entry.entering = false;
        entry.start = entry.target;
    }
}

void ViewTransitionRuntime::BeginFrame() noexcept
{
    if (++generation_ == 0)
    {
        generation_ = 1;
        for (auto& [key, entry] : entries_)
        {
            (void)key;
            entry.generation = 0;
        }
    }
}

ViewTransitionPresentation ViewTransitionRuntime::CurrentPresentation(
    const Entry& entry, TimePoint now) noexcept
{
    if (!entry.active ||
        entry.activeTransition.durationMilliseconds == 0)
        return entry.target;
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::duration<float, std::milli>>(
            now - entry.started).count();
    const float progress = elapsed /
        static_cast<float>(
            entry.activeTransition.durationMilliseconds);
    return InterpolateTransitionPresentation(entry.start, entry.target,
        entry.activeTransition, progress);
}

ViewResolvedTransform ViewTransitionRuntime::CurrentTransform(
    const ViewNode& node, TimePoint now) const
{
    const auto entry = entries_.find(node.key);
    return entry == entries_.end() ? LocalTransform(node)
        : LocalTransform(node.frame, CurrentPresentation(entry->second, now).transform);
}

ViewTransitionPresentation ViewTransitionRuntime::ResolvePresentation(
    std::string_view key, const ViewStyle& targetStyle,
    const std::optional<ViewTransform>& targetTransform,
    const std::optional<ViewTransition>& transition,
    const std::optional<ViewPresenceTransition>& enterTransition,
    TimePoint now, bool reducedMotion)
{
    const ViewTransitionPresentation target{ targetStyle, targetTransform };
    if (key.empty()) return target;
    ViewTransition configured = transition.value_or(ViewTransition{});
    std::erase(configured.properties, ViewTransitionProperty::Layout);
    configured.durationMilliseconds = ScaledDuration(configured.durationMilliseconds);
    auto [position, inserted] = entries_.try_emplace(std::string(key));
    Entry& entry = position->second;
    entry.generation = generation_;
    if (inserted)
    {
        entry.start = target;
        entry.target = target;
        entry.configuredTransition = configured;
        entry.activeTransition = configured;
        if (generation_ > 1 && enterTransition && !reducedMotion)
        {
            ViewTransition entering;
            entering.durationMilliseconds =
                ScaledDuration(enterTransition->durationMilliseconds);
            entering.easing = enterTransition->easing;
            if (enterTransition->opacity)
            {
                entry.start.style.opacity = enterTransition->opacity;
                entering.properties.push_back(
                    ViewTransitionProperty::Opacity);
            }
            if (enterTransition->transform)
            {
                entry.start.transform = enterTransition->transform;
                entering.properties.push_back(
                    ViewTransitionProperty::Transform);
            }
            entry.activeTransition = std::move(entering);
            entry.started = now;
            entry.active = HasTransitionDifference(entry.start,
                entry.target, entry.activeTransition);
            entry.entering = entry.active;
            if (entry.active) return entry.start;
        }
        return target;
    }

    const bool targetChanged = entry.target != target;
    const bool configurationChanged =
        entry.configuredTransition != configured;
    if (targetChanged)
    {
        entry.start = CurrentPresentation(entry, now);
        entry.target = target;
        entry.configuredTransition = configured;
        if (!entry.entering)
            entry.activeTransition = configured;
        entry.started = now;
        entry.active = !reducedMotion &&
            (entry.entering || transition.has_value()) &&
            HasTransitionDifference(
                entry.start, entry.target, entry.activeTransition);
        if (!entry.active) entry.entering = false;
    }
    else if (reducedMotion)
    {
        entry.configuredTransition = configured;
        entry.activeTransition = configured;
        entry.active = false;
        entry.entering = false;
        entry.start = target;
    }
    else if (configurationChanged)
    {
        entry.configuredTransition = configured;
        if (!entry.entering)
        {
            entry.activeTransition = configured;
            entry.active = false;
            entry.start = target;
        }
    }
    if (!entry.active) return target;
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::duration<float, std::milli>>(
            now - entry.started).count();
    if (elapsed >= static_cast<float>(
            entry.activeTransition.durationMilliseconds))
    {
        entry.active = false;
        entry.entering = false;
        entry.start = target;
        return target;
    }
    return InterpolateTransitionPresentation(entry.start, entry.target,
        entry.activeTransition, elapsed /
            static_cast<float>(
                entry.activeTransition.durationMilliseconds));
}

ViewStyle ViewTransitionRuntime::Resolve(std::string_view key,
    const ViewStyle& target,
    const std::optional<ViewTransition>& transition,
    TimePoint now, bool reducedMotion)
{
    return ResolvePresentation(key, target, std::nullopt,
        transition, std::nullopt, now, reducedMotion).style;
}

void ViewTransitionRuntime::CommitScene(
    const ViewNode& previous, const ViewNode& current,
    TimePoint now, bool reducedMotion)
{
    if (reducedMotion)
    {
        exits_.clear();
        layoutAnimation_.reset();
        return;
    }

    std::unordered_set<std::string> currentKeys;
    const auto collectKeys = [&](const auto& self,
        const ViewNode& node) -> void {
        currentKeys.insert(node.key);
        for (const auto& child : node.children) self(self, child);
    };
    collectKeys(collectKeys, current);
    const auto pruneCurrent = [&currentKeys](const auto& self,
        ViewNode& node) -> void {
        std::erase_if(node.children,
            [&currentKeys](const ViewNode& child) {
                return currentKeys.contains(child.key);
            });
        for (auto& child : node.children) self(self, child);
    };
    std::erase_if(exits_, [&currentKeys](const ExitEntry& entry) {
        return currentKeys.contains(entry.node.key);
    });
    const auto countNodes = [](const auto& self,
        const ViewNode& node) -> std::size_t {
        std::size_t count = 1;
        for (const auto& child : node.children)
            count += self(self, child);
        return count;
    };
    std::size_t retainedNodes = 0;
    for (auto& exit : exits_)
    {
        pruneCurrent(pruneCurrent, exit.node);
        exit.nodeCount = countNodes(countNodes, exit.node);
        retainedNodes += exit.nodeCount;
    }
    struct ExitParent
    {
        ViewResolvedTransform transform;
        std::optional<ViewRect> clip;
        const ViewNode* anchor = nullptr;
        ViewResolvedTransform removedAncestorTransform;
        std::optional<ViewRect> removedAncestorClip;
    };
    std::unordered_set<std::string> exitingKeys;
    for (const auto& exit : exits_) exitingKeys.insert(exit.node.key);
    const auto collectExits = [&](const auto& self,
        const ViewNode& node, const ExitParent& parent) -> void {
        if (!node.visible || node.visibility == ViewVisibility::Hidden ||
            node.frame.width <= 0.0f || node.frame.height <= 0.0f)
            return;
        const bool present = currentKeys.contains(node.key);
        if (!present && node.exitTransition &&
            !exitingKeys.contains(node.key))
        {
            ViewTransition descriptor;
            descriptor.durationMilliseconds =
                ScaledDuration(node.exitTransition->durationMilliseconds);
            descriptor.easing = node.exitTransition->easing;
            ViewTransitionPresentation start{ node.style, node.transform };
            if (const auto found = entries_.find(node.key);
                found != entries_.end())
                start = CurrentPresentation(found->second, now);
            ViewTransitionPresentation target = start;
            if (node.exitTransition->opacity)
            {
                target.style.opacity = node.exitTransition->opacity;
                descriptor.properties.push_back(
                    ViewTransitionProperty::Opacity);
            }
            if (node.exitTransition->transform)
            {
                target.transform = node.exitTransition->transform;
                descriptor.properties.push_back(
                    ViewTransitionProperty::Transform);
            }
            bool queued = false;
            if (HasTransitionDifference(start, target, descriptor))
            {
                ExitEntry exit;
                exit.node = node;
                pruneCurrent(pruneCurrent, exit.node);
                exit.nodeCount = countNodes(countNodes, exit.node);
                while (retainedNodes + exit.nodeCount >
                        ViewTreeLimits::MaximumNodes && !exits_.empty())
                {
                    retainedNodes -= exits_.front().nodeCount;
                    exitingKeys.erase(exits_.front().node.key);
                    exits_.erase(exits_.begin());
                }
                if (exit.nodeCount > ViewTreeLimits::MaximumNodes)
                    return;
                exit.parentTransform = parent.transform;
                exit.parentClip = parent.clip;
                if (parent.anchor)
                {
                    exit.anchor = ExitAnchor{ parent.anchor->key,
                        ViewNodeContentRect(*parent.anchor),
                        parent.removedAncestorTransform,
                        parent.removedAncestorClip };
                }
                exit.start = std::move(start);
                exit.target = std::move(target);
                exit.transition = std::move(descriptor);
                exit.started = now;
                retainedNodes += exit.nodeCount;
                exitingKeys.insert(exit.node.key);
                exits_.push_back(std::move(exit));
                queued = true;
            }
            if (queued) return;
        }

        const auto localTransform = CurrentTransform(node, now);
        ExitParent childParent;
        childParent.transform =
            ComposeTransform(localTransform, parent.transform);
        childParent.clip = IntersectExitClips(parent.clip,
            TransformExitClip(node.clipFrame, childParent.transform));
        if (present)
        {
            childParent.anchor = &node;
        }
        else if (parent.anchor)
        {
            childParent.anchor = parent.anchor;
            childParent.removedAncestorTransform = ComposeTransform(
                localTransform, parent.removedAncestorTransform);
            childParent.removedAncestorClip = IntersectExitClips(
                parent.removedAncestorClip, TransformExitClip(
                    node.clipFrame, childParent.removedAncestorTransform));
        }
        for (const auto& child : node.children)
            self(self, child, childParent);
    };
    const auto& previousPaint = layoutAnimation_
        ? layoutAnimation_->LastPainted(previous) : previous;
    collectExits(collectExits, previousPaint, {});

    if (!layoutAnimation_ && detail::HasViewLayoutAnimation(current))
        layoutAnimation_ = std::make_unique<detail::ViewLayoutAnimation>();
    if (layoutAnimation_)
    {
        layoutAnimation_->Commit(previous, current, durationScale_);
        if (!layoutAnimation_->HasActive()) layoutAnimation_.reset();
    }
}

const ViewNode& ViewTransitionRuntime::LayoutForPaint(
    const ViewNode& committed, TimePoint now, bool reducedMotion)
{
    if (!layoutAnimation_) return committed;
    if (!reducedMotion)
    {
        const auto& painted = layoutAnimation_->Paint(committed, now);
        if (layoutAnimation_->HasActive()) return painted;
    }
    layoutAnimation_.reset();
    return committed;
}

std::vector<ViewExitTransitionFrame> ViewTransitionRuntime::ExitFrames(
    const ViewNode& painted, TimePoint now, bool reducedMotion)
{
    std::vector<ViewExitTransitionFrame> frames;
    if (reducedMotion)
    {
        exits_.clear();
        return frames;
    }

    struct PaintPlacement
    {
        const ViewNode* node = nullptr;
        ViewResolvedTransform transform;
        std::optional<ViewRect> clip;
    };
    std::unordered_map<std::string_view, PaintPlacement> anchors;
    for (const auto& exit : exits_)
        if (exit.anchor) anchors.try_emplace(exit.anchor->key);
    if (!anchors.empty())
    {
        const auto collect = [&](const auto& self, const ViewNode& node,
            const PaintPlacement& parent) -> void {
            if (!node.visible || node.visibility == ViewVisibility::Hidden ||
                node.frame.width <= 0.0f || node.frame.height <= 0.0f)
                return;
            const auto transform = ComposeTransform(
                CurrentTransform(node, now), parent.transform);
            const PaintPlacement placement{ &node, transform,
                IntersectExitClips(parent.clip,
                    TransformExitClip(node.clipFrame, transform)) };
            if (auto anchor = anchors.find(node.key); anchor != anchors.end())
                anchor->second = placement;
            for (const auto& child : node.children)
                self(self, child, placement);
        };
        collect(collect, painted, {});
    }

    frames.reserve(exits_.size());
    for (auto& exit : exits_)
    {
        if (exit.anchor)
        {
            const auto& placement = anchors.at(exit.anchor->key);
            if (placement.node)
            {
                const auto& start = exit.anchor->contentFrame;
                const auto target = ViewNodeContentRect(*placement.node);
                ViewResolvedTransform mapping;
                constexpr float epsilon = 0.000001f;
                if (start.width > epsilon && start.height > epsilon)
                {
                    mapping.m11 = target.width / start.width;
                    mapping.m22 = target.height / start.height;
                    mapping.dx = target.x - start.x * mapping.m11;
                    mapping.dy = target.y - start.y * mapping.m22;
                }
                const auto reanchor = ComposeTransform(mapping, placement.transform);
                exit.parentTransform = ComposeTransform(
                    exit.anchor->removedAncestorTransform, reanchor);
                exit.parentClip = IntersectExitClips(placement.clip,
                    TransformExitClip(exit.anchor->removedAncestorClip, reanchor));
            }
        }
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::duration<float, std::milli>>(
                now - exit.started).count();
        const float progress = elapsed /
            static_cast<float>(exit.transition.durationMilliseconds);
        frames.push_back({ &exit.node, exit.parentTransform,
            exit.parentClip,
            InterpolateTransitionPresentation(exit.start, exit.target,
                exit.transition, progress) });
    }
    return frames;
}

void ViewTransitionRuntime::EndFrame()
{
    std::erase_if(entries_, [this](const auto& item) {
        return item.second.generation != generation_;
    });
}

bool ViewTransitionRuntime::Tick(TimePoint now) noexcept
{
    bool hadActive = !exits_.empty() || layoutAnimation_ != nullptr;
    for (auto& [key, entry] : entries_)
    {
        (void)key;
        if (!entry.active) continue;
        hadActive = true;
        if (now - entry.started >= std::chrono::milliseconds(
                entry.activeTransition.durationMilliseconds))
        {
            entry.active = false;
            entry.entering = false;
            entry.start = entry.target;
        }
    }
    std::erase_if(exits_, [now](const ExitEntry& exit) {
        return now - exit.started >= std::chrono::milliseconds(
            exit.transition.durationMilliseconds);
    });
    return hadActive;
}

bool ViewTransitionRuntime::HasActive() const noexcept
{
    return !exits_.empty() || layoutAnimation_ != nullptr ||
        std::any_of(entries_.begin(), entries_.end(),
        [](const auto& item) { return item.second.active; });
}

void ViewTransitionRuntime::Clear() noexcept
{
    entries_.clear();
    exits_.clear();
    layoutAnimation_.reset();
    generation_ = 0;
}

std::size_t ViewTransitionRuntime::Size() const noexcept
{
    return entries_.size() + exits_.size();
}

std::vector<const ViewNode*> ViewChildrenInPaintOrder(const ViewNode& node)
{
    std::vector<const ViewNode*> result;
    result.reserve(node.children.size());
    for (const auto& child : node.children) result.push_back(&child);
    std::stable_sort(result.begin(), result.end(),
        [](const ViewNode* left, const ViewNode* right) {
            if (left->stickyPresented != right->stickyPresented)
                return !left->stickyPresented;
            return left->zIndex < right->zIndex;
        });
    return result;
}

ViewRect ViewRadioOptionFrame(
    const ViewNode& node, std::size_t optionIndex) noexcept
{
    if (node.options.empty() || optionIndex >= node.options.size())
        return {};
    const ViewRect content = ViewNodeContentRect(node);
    const float count = static_cast<float>(node.options.size());
    if (node.orientation == ViewOrientation::Horizontal)
    {
        const float width = std::max(0.0f,
            (content.width - node.gap * (count - 1.0f)) / count);
        return { content.x + static_cast<float>(optionIndex) *
                (width + node.gap), content.y, width, content.height };
    }
    const float height = std::max(0.0f,
        (content.height - node.gap * (count - 1.0f)) / count);
    return { content.x, content.y + static_cast<float>(optionIndex) *
            (height + node.gap), content.width, height };
}

ViewRect ViewSelectPopupFrame(const ViewNode& node,
    float viewportHeight) noexcept
{
    if (node.options.empty() || node.frame.width <= 0.0f)
        return {};
    const float optionHeight = std::max(28.0f,
        std::min(48.0f, node.fontSize * 1.8f));
    const float desiredHeight = optionHeight *
        static_cast<float>(std::min<std::size_t>(node.options.size(), 5));
    const float above = std::max(0.0f, node.frame.y - 4.0f);
    const float below = std::max(0.0f, viewportHeight -
        node.frame.y - node.frame.height - 4.0f);
    const bool openAbove = below < desiredHeight && above > below;
    const float popupHeight = std::min(desiredHeight,
        openAbove ? above : below);
    const float popupTop = openAbove
        ? node.frame.y - popupHeight
        : node.frame.y + node.frame.height;
    return { node.frame.x, popupTop, node.frame.width, popupHeight };
}

ViewRect ViewSelectOptionFrame(const ViewNode& node,
    std::size_t optionIndex, float viewportHeight) noexcept
{
    if (node.options.empty() || optionIndex >= node.options.size())
        return {};
    const float optionHeight = std::max(28.0f,
        std::min(48.0f, node.fontSize * 1.8f));
    const ViewRect popup = ViewSelectPopupFrame(node, viewportHeight);
    return { popup.x, popup.y +
        optionHeight * static_cast<float>(optionIndex) - node.scrollOffset,
        popup.width, optionHeight };
}

ViewRect ViewMonthCalendarWeekdayFrame(
    const ViewNode& node, std::size_t weekdayIndex) noexcept
{
    if (weekdayIndex >= 7) return {};
    const ViewRect content = ViewNodeContentRect(node);
    const float width = content.width / 7.0f;
    const float height = content.height / 7.0f;
    return { content.x + width * static_cast<float>(weekdayIndex),
        content.y, width, height };
}

ViewRect ViewMonthCalendarCellFrame(
    const ViewNode& node, std::size_t cellIndex) noexcept
{
    if (cellIndex >= 42) return {};
    const ViewRect content = ViewNodeContentRect(node);
    const float width = content.width / 7.0f;
    const float headerHeight = content.height / 7.0f;
    const float height = (content.height - headerHeight) / 6.0f;
    const std::size_t column = cellIndex % 7;
    const std::size_t row = cellIndex / 7;
    return { content.x + width * static_cast<float>(column),
        content.y + headerHeight + height * static_cast<float>(row),
        width, height };
}

bool BuildViewMonthCalendarCells(const ViewNode& node,
    std::array<ViewMonthCalendarCell, 42>& cells, std::string& error)
{
    error.clear();
    cells = {};
    if (node.calendarYear < 1 || node.calendarYear > 9999 ||
        node.calendarMonth < 1 || node.calendarMonth > 12 ||
        node.firstDayOfWeek < 1 || node.firstDayOfWeek > 7)
    {
        error = "monthCalendar date fields are out of range";
        return false;
    }
    CivilDate first{ node.calendarYear, node.calendarMonth, 1, 0 };
    const long long firstSerial = DaysFromCivil(first.year,
        static_cast<unsigned>(first.month), 1);
    int firstWeekday = static_cast<int>((firstSerial + 4) % 7);
    if (firstWeekday < 0) firstWeekday += 7;
    first.weekday = firstWeekday + 1;
    const int leading =
        (first.weekday - node.firstDayOfWeek + 7) % 7;

    CivilDate selected;
    if (!node.calendarSelectedDate.empty() &&
        !ParseCivilDate(node.calendarSelectedDate, selected))
    {
        error = "monthCalendar selectedDate must be empty or ISO YYYY-MM-DD";
        return false;
    }
    CivilDate today;
    if (!node.calendarTodayDate.empty() &&
        !ParseCivilDate(node.calendarTodayDate, today))
    {
        error = "monthCalendar todayDate must be empty or ISO YYYY-MM-DD";
        return false;
    }
    std::unordered_set<std::string> eventDates;
    if (node.calendarEventDates.size() >
        ViewTreeLimits::MaximumCalendarEventDates)
    {
        error = "monthCalendar eventDates exceed 366 entries";
        return false;
    }
    for (const auto& value : node.calendarEventDates)
    {
        CivilDate eventDate;
        if (!ParseCivilDate(value, eventDate))
        {
            error = "monthCalendar eventDates must use ISO YYYY-MM-DD";
            return false;
        }
        if (!eventDates.insert(value).second)
        {
            error = "monthCalendar eventDates must be unique";
            return false;
        }
    }

    const long long gridStart = firstSerial - leading;
    for (std::size_t index = 0; index < cells.size(); ++index)
    {
        const CivilDate date = CivilFromDays(
            gridStart + static_cast<long long>(index));
        if (date.year < 1 || date.year > 9999)
        {
            error = "monthCalendar grid falls outside supported dates";
            cells = {};
            return false;
        }
        ViewMonthCalendarCell cell;
        cell.date = FormatCivilDate(date);
        cell.day = date.day;
        cell.currentMonth = date.year == node.calendarYear &&
            date.month == node.calendarMonth;
        cell.selected = cell.date == node.calendarSelectedDate;
        cell.today = cell.date == node.calendarTodayDate;
        cell.hasEvent = eventDates.contains(cell.date);
        cells[index] = std::move(cell);
    }
    return true;
}

bool ValidateAndLayoutViewTree(ViewNode& root, float width, float height,
    std::string& error)
{
    error.clear();
    if (!FiniteInRange(width, 1.0f, MaximumDimension) ||
        !FiniteInRange(height, 1.0f, MaximumDimension))
    {
        error = "view surface dimensions must be finite and positive";
        return false;
    }
    const auto refreshVirtualChildIndices = [&](const auto& self,
        ViewNode& node) -> void {
        node.virtualChildIndices.clear();
        if (IsVirtualCollection(node.type) &&
            node.collectionContent == ViewCollectionContent::Items)
        {
            node.virtualChildIndices.reserve(node.children.size());
            const std::size_t windowChildOffset =
                node.stickyHeaderIndex &&
                *node.stickyHeaderIndex < node.firstIndex &&
                !node.children.empty() ? 1 : 0;
            if (windowChildOffset != 0)
                node.virtualChildIndices.push_back(
                    *node.stickyHeaderIndex);
            for (std::size_t child = windowChildOffset;
                child < node.children.size(); ++child)
                node.virtualChildIndices.push_back(
                    node.firstIndex + child - windowChildOffset);
        }
        for (auto& child : node.children) self(self, child);
    };
    refreshVirtualChildIndices(refreshVirtualChildIndices, root);

    if (!detail::ValidateViewTreeStructure(root, error)) return false;
    ClearCollectionSelectionState(root);
    ApplyCollectionSelectionState(root);
    if (!detail::ResolveGridPlacements(root, error)) return false;
    if (!detail::ValidateAccessibilityMetadata(root, error)) return false;
    detail::LayoutViewTree(root, { 0.0f, 0.0f, width, height });
    return ValidateTransforms(root, {}, error) &&
        ValidateTransforms(root, {}, error, ViewPresencePhase::Enter) &&
        ValidateTransforms(root, {}, error, ViewPresencePhase::Exit);
}

bool ViewLayoutCache::ValidateAndLayout(ViewNode& root,
    float width, float height, std::string& error, bool* reused)
{
    if (reused) *reused = false;
    if (snapshot_ && snapshot_->width == width &&
        snapshot_->height == height && snapshot_->input == root)
    {
        root = snapshot_->output;
        error.clear();
        if (reused) *reused = true;
        return true;
    }

    // Keep the pre-layout input: layout also resolves selection, grid placement
    // and virtual indices, so the committed/presented tree is not a cache key.
    ViewNode input = root;
    if (!ValidateAndLayoutViewTree(root, width, height, error))
    {
        snapshot_.reset();
        return false;
    }
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->input = std::move(input);
    snapshot->output = root;
    snapshot->width = width;
    snapshot->height = height;
    snapshot_ = std::move(snapshot);
    return true;
}

bool ValidateViewLogicalSlots(const ViewNode& root,
    const LogicalSlotModel& model, std::string& error)
{
    error.clear();
    std::unordered_set<std::string> surfaces;
    const auto validate = [&](const auto& self,
        const ViewNode& node) -> bool {
        if (node.type == ViewNodeType::SlotSurface)
        {
            if (!surfaces.insert(node.logicalSlotId).second)
            {
                error = "logical slot has more than one slotSurface: " +
                    node.logicalSlotId;
                return false;
            }
            const LogicalSlotSnapshot* snapshot =
                model.Find(node.logicalSlotId);
            if (!snapshot)
            {
                error = "slotSurface references an undeclared logical slot: " +
                    node.logicalSlotId;
                return false;
            }
            if (snapshot->kind != node.logicalSlotKind)
            {
                error = "slotSurface kind does not match its manifest declaration: " +
                    node.logicalSlotId;
                return false;
            }
            if (node.logicalSlotRevision != 0 &&
                node.logicalSlotRevision != snapshot->revision)
            {
                error = "slotSurface revision is stale: " +
                    node.logicalSlotId;
                return false;
            }

            std::vector<const ViewNode*> items;
            for (const auto& child : node.children)
                if (child.type == ViewNodeType::SlotItem)
                    items.push_back(&child);
            if (snapshot->kind == LogicalSlotKind::Binding)
            {
                if (snapshot->items.empty())
                {
                    if (!items.empty())
                    {
                        error = "empty binding slotSurface cannot report a slotItem: " +
                            node.logicalSlotId;
                        return false;
                    }
                }
                else if (items.size() != 1 || node.children.size() != 1 ||
                    items.front()->key != snapshot->items.front().id ||
                    items.front()->logicalSlotReference !=
                        snapshot->items.front().reference)
                {
                    error = "binding slotSurface must report its exact host item: " +
                        node.logicalSlotId;
                    return false;
                }
            }
            else
            {
                const bool declaredEmptyContent = snapshot->items.empty() &&
                    node.collectionContent == ViewCollectionContent::Empty;
                if ((declaredEmptyContent &&
                        (!items.empty() || node.children.size() != 1)) ||
                    (!declaredEmptyContent &&
                        (items.size() != snapshot->items.size() ||
                         node.children.size() != snapshot->items.size())))
                {
                    error = "collection slotSurface must report every host item: " +
                        node.logicalSlotId;
                    return false;
                }
                for (std::size_t index = 0; index < items.size(); ++index)
                {
                    if (items[index]->key != snapshot->items[index].id ||
                        items[index]->logicalSlotReference !=
                            snapshot->items[index].reference)
                    {
                        error = "collection slotSurface item order or reference is stale: " +
                            node.logicalSlotId;
                        return false;
                    }
                }
            }
        }
        for (const auto& child : node.children)
            if (!self(self, child)) return false;
        return true;
    };
    return validate(validate, root);
}

std::vector<ViewInspectionNode> InspectViewTree(const ViewNode& root)
{
    std::vector<ViewInspectionNode> result;
    result.reserve(ViewTreeLimits::MaximumNodes);
    const auto collect = [&](const auto& self, const ViewNode& node,
                             std::size_t depth) -> void {
        result.push_back({ node.type, node.key, node.debugName,
            node.testId, node.frame, depth });
        for (const auto& child : node.children)
            self(self, child, depth + 1);
    };
    collect(collect, root, 0);
    return result;
}

bool HasVisibleIndeterminateProgress(const ViewNode& root) noexcept
{
    const auto contains = [&](const auto& self, const ViewNode& node) -> bool {
        if (!node.visible || node.visibility != ViewVisibility::Visible ||
            node.frame.width <= 0.0f || node.frame.height <= 0.0f)
            return false;
        if (node.indeterminate &&
            (node.type == ViewNodeType::ProgressBar ||
                node.type == ViewNodeType::ProgressRing))
            return true;
        return std::any_of(node.children.begin(), node.children.end(),
            [&](const ViewNode& child) { return self(self, child); });
    };
    return contains(contains, root);
}

bool ApplyViewScrollOffsets(ViewNode& root,
    const ViewScrollOffsetResolver& resolver,
    std::vector<ViewScrollViewport>& viewports, std::string& error)
{
    error.clear();
    viewports.clear();
    std::size_t scrollContainers = 0;
    if (!ApplyScrollState(root, resolver, viewports, std::nullopt,
            root.frame.height, scrollContainers, error) ||
        !ValidateTransforms(root, {}, error) ||
        !ValidateTransforms(root, {}, error,
            ViewPresencePhase::Enter) ||
        !ValidateTransforms(root, {}, error,
            ViewPresencePhase::Exit))
    {
        viewports.clear();
        return false;
    }
    for (auto& viewport : viewports)
    {
        if (viewport.selectPopup)
        {
            viewport.frame = ApplyViewTransform(viewport.frame,
                ResolveViewTransformForKey(root, viewport.key));
            continue;
        }
        if (const auto clip = ResolveViewClipForKey(
                root, viewport.key, true))
            viewport.frame = *clip;
        else
            viewport.frame = ApplyViewTransform(viewport.frame,
                ResolveViewTransformForKey(root, viewport.key));
    }
    std::erase_if(viewports, [](const auto& viewport) {
        return viewport.frame.width <= 0.0f ||
            viewport.frame.height <= 0.0f;
    });
    return true;
}

void ApplyViewSurfaceScrollbarGutters(ViewNode& root,
    std::vector<ViewScrollViewport>& viewports,
    const ViewRect& contentBounds, const ViewRect& trackBounds)
{
    const auto visit = [&](auto&& self, ViewNode& node) -> void {
        node.scrollbarFrame.reset();
        if (!node.visible || node.visibility != ViewVisibility::Visible ||
            node.transform || node.transition || node.enterTransition ||
            node.exitTransition)
            return;
        if (IsScrollContainer(node.type))
        {
            const auto sameEdge = [](float a, float b) {
                return std::abs(a - b) < 0.5f;
            };
            const bool vertical = node.orientation == ViewOrientation::Vertical;
            const bool atSurfaceEdge = vertical
                ? sameEdge(node.frame.x, contentBounds.x) &&
                    sameEdge(node.frame.width, contentBounds.width) &&
                    node.frame.y >= contentBounds.y - 0.5f &&
                    node.frame.y + node.frame.height <=
                        contentBounds.y + contentBounds.height + 0.5f
                : sameEdge(node.frame.y, contentBounds.y) &&
                    sameEdge(node.frame.height, contentBounds.height) &&
                    node.frame.x >= contentBounds.x - 0.5f &&
                    node.frame.x + node.frame.width <=
                        contentBounds.x + contentBounds.width + 0.5f;
            if (!node.showScrollbar || !node.clipFrame ||
                node.scrollContentExtent <= node.scrollViewportExtent ||
                !atSurfaceEdge)
                return;
            ViewRect track = *node.clipFrame;
            if (vertical)
                track.width = std::max(track.width,
                    trackBounds.x + trackBounds.width - track.x);
            else
                track.height = std::max(track.height,
                    trackBounds.y + trackBounds.height - track.y);
            node.scrollbarFrame = track;
            for (auto& viewport : viewports)
                if (viewport.key == node.key && !viewport.selectPopup)
                    viewport.scrollbarFrame = track;
            return; // A child scrollbar cannot escape this content clip.
        }
        if (node.clipFrame)
            return;
        for (auto& child : node.children)
            self(self, child);
    };
    visit(visit, root);
}

bool ComputeViewVirtualRange(std::size_t itemCount, float itemExtent,
    std::size_t columns, float rowGap, float viewportExtent,
    float requestedOffset, std::size_t overscan,
    ViewVirtualRange& range, std::string& error)
{
    error.clear();
    range = {};
    if (itemCount > ViewTreeLimits::MaximumVirtualItemCount ||
        columns == 0 || columns > 64 ||
        overscan > ViewTreeLimits::MaximumVirtualOverscan ||
        !FiniteInRange(itemExtent, 0.000001f, MaximumScrollExtent) ||
        !FiniteInRange(rowGap, 0.0f, 4096.0f) ||
        !FiniteInRange(viewportExtent, 0.000001f,
            MaximumScrollExtent) ||
        !std::isfinite(requestedOffset))
    {
        error = "virtual range arguments must be finite and within their limits";
        return false;
    }

    const std::size_t rowCount = itemCount == 0 ? 0 :
        (itemCount + columns - 1) / columns;
    const double rawContentExtent =
        static_cast<double>(rowCount) * itemExtent +
        static_cast<double>(rowCount > 0 ? rowCount - 1 : 0) * rowGap;
    if (!std::isfinite(rawContentExtent) ||
        rawContentExtent > MaximumScrollExtent)
    {
        error = "virtual collection content extent exceeds 1000000";
        return false;
    }
    range.viewportExtent = viewportExtent;
    range.contentExtent = std::max(viewportExtent,
        static_cast<float>(rawContentExtent));
    range.maximum = std::max(0.0f,
        range.contentExtent - viewportExtent);
    range.offset = std::clamp(requestedOffset, 0.0f, range.maximum);
    if (itemCount == 0) return true;

    const double stride = static_cast<double>(itemExtent) + rowGap;
    std::size_t firstVisibleRow = std::min(rowCount - 1,
        static_cast<std::size_t>(std::floor(range.offset / stride)));
    const double firstRowEnd =
        static_cast<double>(firstVisibleRow) * stride + itemExtent;
    if (firstRowEnd <= range.offset && firstVisibleRow + 1 < rowCount)
        ++firstVisibleRow;
    const double visibleEnd = std::nextafter(
        static_cast<double>(range.offset) + viewportExtent,
        -std::numeric_limits<double>::infinity());
    std::size_t lastVisibleRow = std::min(rowCount - 1,
        static_cast<std::size_t>(std::floor(
            std::max(0.0, visibleEnd) / stride)));
    if (lastVisibleRow < firstVisibleRow)
        lastVisibleRow = firstVisibleRow;
    const std::size_t firstRow = firstVisibleRow > overscan
        ? firstVisibleRow - overscan : 0;
    const std::size_t lastRow = std::min(rowCount - 1,
        lastVisibleRow + std::min(overscan,
            rowCount - 1 - lastVisibleRow));
    range.firstIndex = firstRow * columns + 1;
    range.lastIndex = std::min(itemCount, (lastRow + 1) * columns);
    if (range.lastIndex - range.firstIndex + 1 >
        ViewTreeLimits::MaximumVirtualWindowItems)
    {
        error = "virtual materialization window exceeds 128 items";
        range = {};
        return false;
    }
    return true;
}

bool ComputeViewVirtualItemScrollOffset(std::size_t itemCount,
    float itemExtent, std::size_t columns, float rowGap,
    float viewportExtent, float currentOffset, std::size_t index,
    std::string_view alignment, float& offset, std::string& error)
{
    offset = 0.0f;
    ViewVirtualRange range;
    if (!ComputeViewVirtualRange(itemCount, itemExtent, columns, rowGap,
            viewportExtent, currentOffset, 0, range, error))
        return false;
    if (index == 0 || index > itemCount)
    {
        error = "virtual item index is outside the collection";
        return false;
    }
    if (alignment != "nearest" && alignment != "start" &&
        alignment != "center" && alignment != "end")
    {
        error = "virtual item alignment must be nearest, start, center, or end";
        return false;
    }
    const std::size_t row = (index - 1) / columns;
    const float itemStart = static_cast<float>(row) *
        (itemExtent + rowGap);
    const float itemEnd = itemStart + itemExtent;
    float requested = range.offset;
    if (alignment == "start") requested = itemStart;
    else if (alignment == "center")
        requested = itemStart - (viewportExtent - itemExtent) * 0.5f;
    else if (alignment == "end")
        requested = itemEnd - viewportExtent;
    else if (itemStart < range.offset) requested = itemStart;
    else if (itemEnd > range.offset + viewportExtent)
        requested = itemEnd - viewportExtent;
    offset = std::clamp(requested, 0.0f, range.maximum);
    return true;
}

namespace
{
bool ValidateVariableVirtualMeasurements(std::size_t itemCount,
    float estimatedItemSize, float rowGap,
    std::span<const ViewVirtualItemMeasurement> measurements,
    float& contentExtent, std::string& error)
{
    if (itemCount > ViewTreeLimits::MaximumVirtualItemCount ||
        measurements.size() > 4096 ||
        !FiniteInRange(estimatedItemSize, 0.000001f,
            MaximumScrollExtent) ||
        !FiniteInRange(rowGap, 0.0f, 4096.0f))
    {
        error = "variable virtual collection arguments exceed their limits";
        return false;
    }
    std::unordered_set<std::size_t> indices;
    double total = static_cast<double>(itemCount) * estimatedItemSize +
        static_cast<double>(itemCount > 0 ? itemCount - 1 : 0) * rowGap;
    for (const auto& measurement : measurements)
    {
        if (measurement.index == 0 || measurement.index > itemCount ||
            !FiniteInRange(measurement.extent, 0.000001f,
                MaximumScrollExtent) ||
            !indices.insert(measurement.index).second)
        {
            error = "variable virtual measurements must use unique bounded 1-based indices";
            return false;
        }
        total += static_cast<double>(measurement.extent) -
            estimatedItemSize;
    }
    if (!std::isfinite(total) || total < 0.0 ||
        total > MaximumScrollExtent)
    {
        error = "variable virtual collection content extent exceeds 1000000";
        return false;
    }
    contentExtent = static_cast<float>(total);
    return true;
}

float VariableVirtualExtent(std::size_t index, float estimatedItemSize,
    std::span<const ViewVirtualItemMeasurement> measurements) noexcept
{
    const auto found = std::find_if(measurements.begin(), measurements.end(),
        [index](const auto& measurement) {
            return measurement.index == index;
        });
    return found == measurements.end()
        ? estimatedItemSize : found->extent;
}

float VariableVirtualStart(std::size_t index, float estimatedItemSize,
    float rowGap,
    std::span<const ViewVirtualItemMeasurement> measurements) noexcept
{
    double start = static_cast<double>(index - 1) *
        (estimatedItemSize + rowGap);
    for (const auto& measurement : measurements)
    {
        if (measurement.index < index)
            start += static_cast<double>(measurement.extent) -
                estimatedItemSize;
    }
    return static_cast<float>(start);
}
}

bool ComputeViewVariableVirtualRange(std::size_t itemCount,
    float estimatedItemSize, float rowGap, float viewportExtent,
    float requestedOffset, std::size_t overscan,
    std::span<const ViewVirtualItemMeasurement> measurements,
    ViewVirtualRange& range, std::string& error)
{
    error.clear();
    range = {};
    float logicalContentExtent = 0.0f;
    if (overscan > ViewTreeLimits::MaximumVirtualOverscan ||
        !FiniteInRange(viewportExtent, 0.000001f,
            MaximumScrollExtent) ||
        !std::isfinite(requestedOffset) ||
        !ValidateVariableVirtualMeasurements(itemCount,
            estimatedItemSize, rowGap, measurements,
            logicalContentExtent, error))
    {
        if (error.empty())
            error = "variable virtual range arguments must be finite and within their limits";
        return false;
    }
    range.viewportExtent = viewportExtent;
    range.contentExtent = std::max(viewportExtent, logicalContentExtent);
    range.maximum = std::max(0.0f,
        range.contentExtent - viewportExtent);
    range.offset = std::clamp(requestedOffset, 0.0f, range.maximum);
    if (itemCount == 0) return true;

    std::size_t low = 1;
    std::size_t high = itemCount;
    while (low < high)
    {
        const std::size_t middle = low + (high - low) / 2;
        const float end = VariableVirtualStart(middle,
            estimatedItemSize, rowGap, measurements) +
            VariableVirtualExtent(middle, estimatedItemSize, measurements);
        if (end <= range.offset) low = middle + 1;
        else high = middle;
    }
    const std::size_t firstVisible = low;
    const double visibleEnd = std::nextafter(
        static_cast<double>(range.offset) + viewportExtent,
        -std::numeric_limits<double>::infinity());
    low = firstVisible;
    high = itemCount + 1;
    while (low < high)
    {
        const std::size_t middle = low + (high - low) / 2;
        if (middle > itemCount ||
            VariableVirtualStart(middle, estimatedItemSize,
                rowGap, measurements) > visibleEnd)
            high = middle;
        else
            low = middle + 1;
    }
    const std::size_t lastVisible = std::max(firstVisible, low - 1);
    range.firstIndex = firstVisible > overscan
        ? firstVisible - overscan : 1;
    range.lastIndex = std::min(itemCount,
        lastVisible + std::min(overscan, itemCount - lastVisible));
    if (range.lastIndex - range.firstIndex + 1 >
        ViewTreeLimits::MaximumVirtualWindowItems)
    {
        error = "variable virtual materialization window exceeds 128 items";
        range = {};
        return false;
    }
    return true;
}

bool ComputeViewVariableVirtualItemStart(std::size_t itemCount,
    float estimatedItemSize, float rowGap, std::size_t index,
    std::span<const ViewVirtualItemMeasurement> measurements,
    float& start, std::string& error)
{
    start = 0.0f;
    float contentExtent = 0.0f;
    if (!ValidateVariableVirtualMeasurements(itemCount,
            estimatedItemSize, rowGap, measurements,
            contentExtent, error))
        return false;
    if (index == 0 || index > itemCount)
    {
        error = "variable virtual item index is outside the collection";
        return false;
    }
    start = VariableVirtualStart(index, estimatedItemSize,
        rowGap, measurements);
    return true;
}

bool ComputeViewVariableVirtualItemScrollOffset(std::size_t itemCount,
    float estimatedItemSize, float rowGap, float viewportExtent,
    float currentOffset, std::size_t index, std::string_view alignment,
    std::span<const ViewVirtualItemMeasurement> measurements,
    float& offset, std::string& error)
{
    offset = 0.0f;
    ViewVirtualRange range;
    if (!ComputeViewVariableVirtualRange(itemCount, estimatedItemSize,
            rowGap, viewportExtent, currentOffset, 0,
            measurements, range, error))
        return false;
    if (index == 0 || index > itemCount)
    {
        error = "variable virtual item index is outside the collection";
        return false;
    }
    if (alignment != "nearest" && alignment != "start" &&
        alignment != "center" && alignment != "end")
    {
        error = "variable virtual item alignment must be nearest, start, center, or end";
        return false;
    }
    const float itemStart = VariableVirtualStart(index,
        estimatedItemSize, rowGap, measurements);
    const float itemExtent = VariableVirtualExtent(index,
        estimatedItemSize, measurements);
    const float itemEnd = itemStart + itemExtent;
    float requested = range.offset;
    if (alignment == "start") requested = itemStart;
    else if (alignment == "center")
        requested = itemStart - (viewportExtent - itemExtent) * 0.5f;
    else if (alignment == "end") requested = itemEnd - viewportExtent;
    else if (itemStart < range.offset) requested = itemStart;
    else if (itemEnd > range.offset + viewportExtent)
        requested = itemEnd - viewportExtent;
    offset = std::clamp(requested, 0.0f, range.maximum);
    return true;
}

const char* ViewNodeTypeName(ViewNodeType type) noexcept
{
    const ViewNodeContract* contract = FindViewNodeContract(type);
    return contract ? contract->name.data() : "unknown";
}
}
