local p=module.require("modules/providers.lua")
local j=module.require("modules/json.lua")
local client=module.require("modules/client.lua")
local deepseek=[[{"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"43.16","granted_balance":"3.16","topped_up_balance":"40.00"}]}]]
local function parse(id,body,cfg) return p.parse(id,j.decode(body),cfg) end
local function model()
    local cfg={identity="deepseek:keyA",provider="deepseek",url=p.presets.deepseek.url,
        secretRef="secret:v1:"..string.rep("a",32),interval=300}
    local requests,canceled={},{};local mono,allowed=1000,true
    local m=client.new({config=function()return cfg end,permission=function()return allowed end,
        monotonic=function()return mono end,now=function()return mono+1000000 end,
        start=function(args)requests[#requests+1]=args;return #requests end,
        cancel=function(id)canceled[#canceled+1]=id end,decode=j.decode,parse=p.parse,safeUrl=p.safeUrl})
    local function clock(value,permission) mono=value;if permission~=nil then allowed=permission end end
    local function complete(id,body) return m:complete({taskId=id,ok=true,value={status=200,body=body or deepseek}}) end
    return m,cfg,requests,canceled,clock,complete
end
return {
    ["DeepSeek preserves zero, currency and provider availability; missing values never become zero"]=function()
        local d=parse("deepseek",deepseek);assert(d.remaining==43.16 and d.currency=="CNY" and d.metrics[1].value==40)
        d=parse("deepseek",[[{"is_available":false,"balance_infos":[{"currency":"USD","total_balance":"0","granted_balance":"0","topped_up_balance":"0"}]}]])
        assert(d.remaining==0 and d.available==false and d.currency=="USD")
        for _,body in ipairs({'{}','{"is_available":true,"balance_infos":[]}',
            '{"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":null}]}',
            '{"error":{"message":"bad key"}}'}) do assert(not parse("deepseek",body)) end
        assert(not p.number("NaN") and not p.number(true) and not p.number("1e309"))
    end,
    ["OpenRouter account credits and key quotas remain separate and null alone means no key limit"]=function()
        local d=parse("openrouter",'{"data":{"total_credits":100,"total_usage":56.84}}')
        assert(math.abs(d.remaining-43.16)<0.00000001 and d.currency=="USD")
        d=parse("openrouter_key",'{"data":{"limit":100,"limit_remaining":73,"usage":27}}')
        assert(d.windows[1].remaining==73 and d.windows[1].total==100 and d.windows[1].percent==73)
        assert(parse("openrouter_key",'{"data":{"limit":null,"usage":12}}').unlimited)
        assert(not parse("openrouter_key",'{"data":{"usage":12}}'))
        assert(not parse("openrouter_key",'{"data":{"limit":{},"usage":12}}'))
        assert(not parse("openrouter_key",'{"data":{"limit":100,"usage":12}}'))
        assert(not parse("openrouter",'{"data":{"total_credits":100}}'))
    end,
    ["Official Moonshot and Novita units are honored; undocumented StepFun currency stays unset"]=function()
        local body='{"code":0,"status":true,"data":{"available_balance":46.58893,"cash_balance":-3,"voucher_balance":46.58893}}'
        assert(parse("moonshot",body).currency=="CNY" and parse("moonshot_intl",body).currency=="USD")
        assert(parse("moonshot",body).remaining==46.58893 and parse("moonshot",body).metrics[1].value==-3)
        assert(not parse("moonshot",'{"code":123,"status":false,"data":{"available_balance":43}}'))
        local d=parse("novita",'{"availableBalance":"1000000","cashBalance":"800000","creditLimit":"200000"}')
        assert(d.remaining==100 and d.metrics[1].value==80 and d.metrics[2].value==20 and d.currency=="USD")
        d=parse("stepfun",'{"object":"account","balance":0,"total_cash_balance":12,"total_voucher_balance":26}')
        assert(d.remaining==0 and d.currency==nil and d.metrics[2].value==26)
        assert(not parse("stepfun",'{"balance":43}'))
    end,
    ["Custom reads bounded JSON paths and explicit units; invalid totals and insecure endpoints are rejected"]=function()
        local d=parse("custom",'{"data":{"items":[{"left":"4316","total":10000}]}}',
            {kind="quota",remainingPath="data.items[0].left",totalPath="data.items[0].total",scale=0.01})
        assert(math.abs(d.windows[1].remaining-43.16)<0.00000001 and d.windows[1].total==100)
        assert(not parse("custom",'{"left":110,"total":100}',{kind="quota",remainingPath="left",totalPath="total"}))
        assert(not p.path({data={}},"data.;os.execute()") and not p.path({},string.rep("x",257)))
        assert(p.safeUrl("https://api.example.com:443/v1/balance?account=a"))
        for _,url in ipairs({"http://api.example.com/","https://key@example.com/","https://example.com/#secret","https://example.com\\evil","file:///C:/key"}) do assert(not p.safeUrl(url)) end
        assert(not p.parse("removed-provider",{}))
    end,
    ["Requests use only secret references and cannot overlap; account change cancels and discards old completion"]=function()
        local m,c,requests,canceled,_,complete=model();assert(m:refresh(false))
        local a=requests[1];assert(a.method=="GET" and a.cacheSeconds==0 and a.maxBytes==131072)
        assert(a.headers.Authorization.secretRef==c.secretRef and a.headers.Authorization.prefix=="Bearer ")
        assert(not m:refresh(true) and #requests==1)
        c.identity="moonshot:keyB";c.provider="moonshot";c.url=p.presets.moonshot.url
        assert(m:refresh(false) and canceled[1]==1 and not m.data and not complete(1))
        assert(complete(2,'{"code":0,"status":true,"data":{"available_balance":12}}') and m.data.remaining==12)
    end,
    ["Opaque-reference password replacement clears data and cancels in-flight requests after reset"]=function()
        local m,_,requests,canceled,clock,complete=model();m:refresh(false);complete(1);clock(7000)
        m:refresh(true);assert(m.data.remaining==43.16)
        m:reset();assert(canceled[1]==2 and not m.data and m:refresh(false))
        assert(not complete(2) and #requests==3)
        assert(complete(3) and m.data.remaining==43.16)
    end,
    ["Saved settings immediately replace requests and unblock stable-reference key edits"]=function()
        local m,c,requests,canceled,clock,complete=model();m:refresh(false);complete(1);clock(7000)
        m:refresh(true);m:complete({taskId=2,ok=false,error="httpStatus",status=401})
        assert(m.blocked and m:settingsChanged({"key_deepseek"}) and #requests==3 and not m.data)
        assert(m:settingsChanged({"interval"}) and canceled[1]==3 and not complete(3))
        assert(complete(4) and m.data.remaining==43.16)
        assert(not m:settingsChanged({"mode","lowBalance","key_moonshot"}) and #requests==4)
        c.identity="custom:new-endpoint";c.provider="custom";c.url="https://api.example.com/balance"
        assert(m:settingsChanged({"provider","endpoint"}) and #requests==5 and not m.data)
    end,
    ["A host start exception leaves setup recoverable when saved settings change"]=function()
        local m,_,requests,_,_,complete=model()
        local start=m.deps.start
        m.deps.start=function()error("header secret descriptor is invalid")end
        assert(not m:refresh(false) and m.error=="configure" and not m.loading)
        m.deps.start=start
        assert(m:settingsChanged({"key_deepseek"}) and #requests==1 and complete(1))
        assert(m.data.remaining==43.16 and not m.error)
    end,
    ["Network failures mark stale data and back off; authorization failure pauses until manual retry or key change"]=function()
        local m,_,requests,_,clock,complete=model();m:refresh(false);complete(1);clock(7000);m:refresh(true)
        m:complete({taskId=2,ok=false,error="httpStatus",status=429})
        assert(m.data.remaining==43.16 and m.error=="rate")
        clock(66000);assert(not m:refresh(false));clock(67000);assert(m:refresh(false))
        m:complete({taskId=3,ok=false,error="httpStatus",status=401});clock(1000000)
        assert(not m:refresh(false) and m.error=="auth" and #requests==3)
        assert(m:refresh(true));m:complete({taskId=4,ok=true,value={status=200,body="{broken"}})
        assert(m.error=="invalid_data" and m.data.remaining==43.16)
    end,
    ["Missing plaintext/secret, denied permission and disposal cannot issue or commit a request"]=function()
        local m,c,requests,canceled,clock,complete=model();c.secretRef="sk-plaintext"
        assert(not m:refresh(false) and m.error=="key_missing" and #requests==0)
        c.secretRef="secret:v1:"..string.rep("b",32);clock(1000,false)
        assert(not m:refresh(false) and m.error=="permission" and #requests==0)
        clock(2000,true);m:refresh(false);clock(3000,false);m:refresh(false)
        assert(canceled[1]==1 and not complete(1))
        clock(8000,true);m:refresh(false);m:dispose()
        assert(canceled[2]==2 and not complete(2) and not m:refresh(true))
    end,
}
