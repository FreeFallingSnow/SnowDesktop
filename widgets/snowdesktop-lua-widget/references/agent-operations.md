# Agent operations for Lua components

Give each declared setting an English `agentDescription` of up to 2048 UTF-8
bytes. This text goes to the host's settings catalog. Explain the behavior
implemented by the component, including what an edit changes, where it applies,
and when another setting or layout takes precedence. Keep it beside the setting
in the Lua definition or manifest. The host handles the instance, visibility,
permissions and current value through its existing settings service.

A component can also declare actions on its `widget.define` descriptor.
For an old host, use `widget.hasFeature("agent.actions")` to choose a fallback
or add `agent.actions` to the manifest's `requiredFeatures` if the package
depends on it. The English settings annotation has the feature
`agent.settingDescriptions`. Both use Lua API v2.

```lua
local function event(context, model, value)
    if value.kind == "agent.action" and value.id == "set-caption" then
        storage.set("caption", value.arguments.text)
    end
end

return widget.define({
    render = function()
        draw.text(8, 8, storage.get("caption") or "", 14, 0xFFFFFF)
    end,
    event = event,
    agentActions = {
        ["set-caption"] = {
            description = "Replace this component's saved caption.",
            arguments = {
                text = {
                    type = "string",
                    description = "The complete replacement caption."
                }
            }
        }
    }
})
```

Use an object keyed by stable action IDs. Each action has an English
`description` and optional `arguments` keyed by argument ID. Every argument
declares a `type` of `string`, `boolean`, `number` or `integer`, an English
`description` and optional `required` (defaults to true). Names contain at
most 64 ASCII letters, digits, hyphens, dots or underscores. A component can
declare up to 16 actions and 16 arguments per action, subject to the host's
16 KiB action declaration budget. Each description is at most 1024 UTF-8 bytes
and each supplied string argument at most 4096 UTF-8 bytes.

The host queries the loaded instance, checks the global access switch,
rejects unknown IDs and invalid arguments, and calls its normal event
handler with `kind = "agent.action"`, the action `id`, `source = "agent"`,
`trustedGesture = false`, and `arguments`. Dispatching an agent action grants
no additional permissions. The reply reports successful event delivery.
Expose only actions with a documented, observable product result.
