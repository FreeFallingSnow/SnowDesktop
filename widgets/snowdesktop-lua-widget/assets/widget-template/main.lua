-- Copy this directory as a complete SnowDesktop API v2 widget package.
-- The host owns the outer material. This view draws content only.

local function buildView(context, _model)
    local small = context.sizeClass == "small"
    local padding = math.max(layout.cu(10), math.min(
        layout.cu(14), layout.vmin(5)))

    return view.column({
        key = "template.surface",
        width = "fill",
        height = "fill",
        padding = padding,
        gap = layout.cu(5),
        alignItems = "stretch",
        justifyContent = "center",
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.template.name"),
        },
        children = {
            view.text({
                key = "template.label",
                text = l10n.tr("lua_widget.template.preview_label"),
                fontSize = layout.fontCu(11),
                bold = true,
                maxLines = 1,
                overflowText = "ellipsis",
                style = { foreground = "textSecondary" },
            }),
            view.text({
                key = "template.message",
                text = storage.get("message") or
                    l10n.tr("lua_widget.template.preview_message"),
                fontSize = small and layout.fontCu(14) or layout.fontCu(16),
                bold = true,
                textWrap = "wrap",
                maxLines = small and 2 or 4,
                overflowText = "ellipsis",
                style = { foreground = "textPrimary" },
            }),
        },
    })
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
