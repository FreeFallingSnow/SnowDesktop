local sources = module.require("modules/monitor_sources.lua")

local function fixture()
    local f = { handles = {}, cards = {}, denied = {}, missing = {},
        created = {}, removed = {} }
    function f.reconcile()
        f.gpuDetails = sources.reconcile(f.handles,
            function(card) return f.cards[card] == true end,
            function(feature) return not f.missing[feature] end,
            function(permission) return not f.denied[permission] end,
            function(topic, options)
                f.created[topic] = (f.created[topic] or 0) + 1
                return {
                    options = options,
                    unsubscribe = function()
                        f.removed[topic] = (f.removed[topic] or 0) + 1
                    end,
                }
            end, f.gpuDetails)
    end
    return f
end

return {
    ["GPU and VRAM share one subscription until both cards close"] = function()
        local f = fixture()
        f.cards.gpu, f.cards.vram = true, true
        f.reconcile()
        f.reconcile()
        assert(f.created["system.gpu"] == 1)
        assert(f.handles.gpu.options.whenHidden == "pause")
        assert(f.handles.gpu.options.includeDetails == true)
        f.cards.gpu = false
        f.reconcile()
        assert(f.handles.gpu and not f.removed["system.gpu"])
        f.cards.vram = false
        f.reconcile()
        assert(not f.handles.gpu and f.removed["system.gpu"] == 1)
        f.cards.gpu = true
        f.reconcile()
        assert(f.created["system.gpu"] == 2)
    end,
    ["hidden cards do not acquire sources and network releases both topics"] = function()
        local f = fixture()
        f.reconcile()
        assert(next(f.handles) == nil and next(f.created) == nil)
        f.cards.network = true
        f.reconcile()
        assert(f.handles.network and f.handles.networkStatus)
        f.cards.network = false
        f.reconcile()
        assert(next(f.handles) == nil)
        assert(f.removed["system.network.traffic"] == 1)
        assert(f.removed["system.network.status"] == 1)
    end,
    ["GPU details are probed and an environment change replaces the options"] = function()
        local f = fixture()
        f.cards.vram = true
        f.missing["data.system.gpu.details"] = true
        f.reconcile()
        assert(f.handles.gpu.options.includeDetails == nil)
        f.missing["data.system.gpu.details"] = nil
        f.reconcile()
        assert(f.removed["system.gpu"] == 1 and f.created["system.gpu"] == 2)
        assert(f.handles.gpu.options.includeDetails == true)
        f.reconcile()
        assert(f.created["system.gpu"] == 2)
        f.missing["data.system.gpu.details"] = true
        f.reconcile()
        assert(f.removed["system.gpu"] == 2 and f.created["system.gpu"] == 3)
        assert(f.handles.gpu.options.includeDetails == nil)
    end,
    ["independent subscription sets retain their own GPU capability state"] = function()
        local detailed, legacy = fixture(), fixture()
        detailed.cards.gpu, legacy.cards.gpu = true, true
        legacy.missing["data.system.gpu.details"] = true
        detailed.reconcile()
        legacy.reconcile()
        detailed.reconcile()
        legacy.reconcile()
        assert(detailed.created["system.gpu"] == 1)
        assert(legacy.created["system.gpu"] == 1)
        assert(detailed.handles.gpu.options.includeDetails == true)
        assert(legacy.handles.gpu.options.includeDetails == nil)
        legacy.missing["data.system.gpu.details"] = nil
        legacy.reconcile()
        detailed.reconcile()
        assert(legacy.created["system.gpu"] == 2)
        assert(legacy.removed["system.gpu"] == 1)
        assert(detailed.created["system.gpu"] == 1)
        assert(not detailed.removed["system.gpu"])
        -- The handle table contains no capability metadata for dispose to
        -- mistake for a live subscription.
        for _, handle in pairs(detailed.handles) do
            handle:unsubscribe()
        end
        assert(detailed.removed["system.gpu"] == 1)
    end,
    ["a disposed subscription set leaves no state in a replacement instance"] = function()
        local first = fixture()
        first.cards.gpu = true
        first.reconcile()
        for _, handle in pairs(first.handles) do handle:unsubscribe() end
        first.handles, first.gpuDetails = {}, nil
        first.missing["data.system.gpu.details"] = true
        first.reconcile()
        assert(first.created["system.gpu"] == 2)
        assert(first.removed["system.gpu"] == 1)
        assert(first.handles.gpu.options.includeDetails == nil)
        local replacement = fixture()
        replacement.cards.gpu = true
        replacement.reconcile()
        assert(replacement.created["system.gpu"] == 1)
        assert(replacement.handles.gpu.options.includeDetails == true)
        first.reconcile()
        assert(first.created["system.gpu"] == 2)
    end,
    ["permission and capability changes reconcile active sources"] = function()
        local f = fixture()
        f.cards.disk_io = true
        f.reconcile()
        assert(f.handles.diskIo)
        f.denied["system.storage.read"] = true
        f.reconcile()
        assert(not f.handles.diskIo)
        f.denied["system.storage.read"] = false
        f.missing["data.system.storage.io"] = true
        f.reconcile()
        assert(not f.handles.diskIo)
        f.missing["data.system.storage.io"] = false
        f.reconcile()
        assert(f.created["system.storage.io"] == 2)
    end,
    ["unset card values respect defaults and persisted false values"] = function()
        assert(sources.cardShown("cpu", nil))
        assert(not sources.cardShown("disk_io", nil))
        assert(not sources.cardShown("uptime", nil))
        assert(not sources.cardShown("cpu", false))
        assert(not sources.cardShown("cpu", "false"))
        assert(not sources.cardShown("cpu", "0"))
        assert(sources.cardShown("disk_io", true))
    end,
}
