-- Copy this directory as a complete SnowDesktop API v2 widget package.
-- The host owns the outer material. This view draws content only.

local function buildView(context, _model)
    local width = context.layoutSize.width
    local height = context.layoutSize.height
    local vertical = height > width * 1.05
    local padding = layout.rpx(9)
    local gap = layout.rpx(6)
    local labelFont = layout.rpx(10)
    local messageFont = layout.rpx(14)
    local contentWidth = width - padding * 2
    local labelWidth = vertical and "fill" or contentWidth * 0.30

    local children = {
        view.text({
            key = "template.label",
            text = l10n.tr("lua_widget.template.preview_label"),
            width = labelWidth,
            height = vertical and labelFont * 1.7 or "fill",
            fontSize = labelFont,
            bold = true,
            maxLines = 1,
            overflowText = "ellipsis",
            verticalAlign = "center",
            style = { foreground = "textSecondary" },
        }),
        view.text({
            key = "template.message",
            text = storage.get("message") or
                l10n.tr("lua_widget.template.preview_message"),
            width = "fill",
            height = vertical and messageFont * 3.0 or "fill",
            fontSize = messageFont,
            bold = true,
            textWrap = "wrap",
            maxLines = vertical and 3 or 2,
            overflowText = "ellipsis",
            verticalAlign = "center",
            style = { foreground = "textPrimary" },
        }),
    }

    local content = {
        key = "template.surface",
        width = "fill",
        height = "fill",
        padding = padding,
        gap = gap,
        alignItems = "stretch",
        justifyContent = "center",
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.template.name"),
        },
        children = children,
    }
    return vertical and view.column(content) or view.row(content)
end

return widget.define({
    name = l10n.tr("lua_widget.template.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bg = 0x18202A,
    border = 0xFFFFFF,
    alpha = 0.42,
    borderAlpha = 0.18,
    gradientEndA = 0.28,
    view = buildView,
})
