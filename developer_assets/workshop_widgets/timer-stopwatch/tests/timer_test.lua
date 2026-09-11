local M=module.require("modules/timer.lua")
return {
 ["delayed callbacks finish once without accumulating tick drift"]=function()
    local t=M.new().countdown;t.value=60000;M.start(t,100)
    assert(M.value(t,30100,true)==30000)
    assert(M.tick(t,90000));assert(t.done and not t.running)
    assert(not M.tick(t,100000));assert(M.value(t,100000,true)==0)
 end,
 ["pause excludes paused time and modes retain independent progress"]=function()
    local m=M.new();m.countdown.value=60000;M.start(m.countdown,1000);M.start(m.stopwatch,1000)
    M.pause(m.stopwatch,2250,false);assert(m.stopwatch.value==1250)
    assert(M.value(m.countdown,11000,true)==50000)
    M.start(m.stopwatch,21000);assert(M.value(m.stopwatch,22500,false)==2750)
    M.reset(m.countdown);assert(M.value(m.stopwatch,23000,false)==3250)
 end,
 ["duration accepts minutes or mm:ss and rejects invalid or excessive values"]=function()
    assert(M.parse("5")==300000);assert(M.parse(" 1:30 ")==90000);assert(M.parse("0:01")==1000)
    assert(M.parse("5999:59")==359999000)
    for _,s in ipairs({"0","-1","1:60","1:2","1.5","6000","","abc","1:00:00"}) do assert(M.parse(s)==nil,s) end
 end,
 ["countdown rounds up while stopwatch rounds down and hours remain complete"]=function()
    assert(M.format(1,true)=="00:01");assert(M.format(999,false)=="00:00")
    assert(M.format(3600000,true)=="01:00:00")
 end,
}
