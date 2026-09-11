local navigation=module.require("modules/navigation.lua")
return {
    ["subpages share one host panel until it closes"] = function()
        local opens,leaves,page=0,0,nil
        local nav=navigation.new({openPanel=function() opens=opens+1 end,
            changePage=function(p) page=p end,leavePage=function() leaves=leaves+1 end})
        nav:show("manage");nav:show("choose");nav:show("manage");nav:show("create")
        assert(opens==1 and page=="create")
        nav:closed();assert(leaves==1)
        nav:show("choose");assert(opens==2 and page=="choose")
        nav:dispose();nav:show("manage");assert(opens==2 and page=="choose")
    end,
}
