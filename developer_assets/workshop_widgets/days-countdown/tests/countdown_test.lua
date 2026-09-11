local logic=module.require("modules/countdown.lua")
local function fixture()
    local data={available=true,value={events={{id="one",title="Original",date="2026-09-11"}}}}
    local f={id="one",read=true,write=true,starts=0,cancels=0,closed=0,data=data}
    f.link=logic.new({getId=function() return f.id end,setId=function(id) f.id=id end,
        canRead=function() return f.read end,canWrite=function() return f.write end,
        subscribe=function(options)
            assert(options.eventId==f.id and not options.fromDate)
            return {value=function() return f.data end,unsubscribe=function() f.closed=f.closed+1 end}
        end,
        start=function(args)
            assert(args.allDay and args.reminderMinutes==-1 and args.title=="New")
            f.starts=f.starts+1;return 42
        end,cancel=function() f.cancels=f.cancels+1 end})
    return f
end
return {
    ["civil days cover leap centuries and signed boundaries"] = function()
        assert(logic.difference("2026-09-12","2026-09-11")==1)
        assert(logic.difference("2026-09-11","2026-09-11")==0)
        assert(logic.difference("2025-12-31","2026-01-01")==-1)
        assert(logic.difference("2000-03-01","2000-02-28")==2)
        assert(logic.difference("1900-03-01","1900-02-28")==1)
        assert(logic.difference("2026-03-09","2026-03-08")==1)
        assert(not logic.parts("2025-02-29") and not logic.parts("0000-01-01"))
        assert(not logic.parts("2026-13-01") and not logic.parts("2026-01-00"))
    end,
    ["linked events follow changes and distinguish missing from unreadable"] = function()
        local f=fixture();local status,item=f.link:current();assert(status=="ready" and item.title=="Original")
        f.data.value.events[1]={id="one",title="Changed",date="2035-02-28"}
        status,item=f.link:current();assert(status=="ready" and item.date=="2035-02-28" and item.title=="Changed")
        f.data.available=false;assert(f.link:current()=="unavailable")
        f.read=false;assert(f.link:current()=="permission")
        f.read=true;f.data.available=true;f.data.value.events={};assert(f.link:current()=="deleted")
    end,
    ["unlink preserves source and isolates instances"] = function()
        local a,b=fixture(),fixture();assert(a.link:bind(""));assert(a.id=="" and b.id=="one")
        assert(#a.data.value.events==1 and a.starts==0 and a.link:current()=="empty")
    end,
    ["creation guards validation permission duplicate submits and completion ownership"] = function()
        local f=fixture();assert(not f.link:create("", "2026-09-11"))
        assert(not f.link:create("New", "2026-02-30"))
        f.write=false;assert(not f.link:create("New", "2026-09-11"));assert(f.starts==0)
        f.write=true;assert(f.link:create("New", "2026-09-11"))
        assert(not f.link:create("New", "2026-09-11") and f.starts==1)
        assert(not f.link:complete({taskId=17,ok=true,value={id="wrong"}}));assert(f.id=="one")
        assert(not f.link:complete({taskId=42,ok=false,error="save_failed"}));assert(f.id=="one")
        assert(f.link:create("New", "2026-09-11"))
        assert(f.link:complete({taskId=42,ok=true,value={id="created"}}));assert(f.id=="created")
    end,
    ["destroyed instance cannot bind a late completion"] = function()
        local f=fixture();assert(f.link:create("New", "2026-09-11"));f.link:dispose()
        assert(f.cancels==1 and f.closed==1)
        assert(not f.link:complete({taskId=42,ok=true,value={id="late"}}));assert(f.id=="one")
    end,
}
