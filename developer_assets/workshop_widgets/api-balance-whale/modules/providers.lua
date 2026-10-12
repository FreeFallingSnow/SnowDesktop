-- Public documented APIs only. See README.md for official field/unit references.
local json=module.require("modules/json.lua")
local M = {}
M.ids = {"deepseek", "openrouter", "openrouter_key", "moonshot", "moonshot_intl", "stepfun", "novita",
    "custom"}
M.presets = {
    deepseek={name="DeepSeek",url="https://api.deepseek.com/user/balance",currency="CNY",console="https://platform.deepseek.com/"},
    openrouter={name="OpenRouter",url="https://openrouter.ai/api/v1/credits",currency="USD",console="https://openrouter.ai/settings/credits"},
    openrouter_key={name="OpenRouter · Key",url="https://openrouter.ai/api/v1/key",currency="USD",kind="quota",console="https://openrouter.ai/settings/keys"},
    moonshot={name="Kimi / Moonshot · CN",url="https://api.moonshot.cn/v1/users/me/balance",currency="CNY",console="https://platform.kimi.com/"},
    moonshot_intl={name="Kimi / Moonshot · INTL",url="https://api.moonshot.ai/v1/users/me/balance",currency="USD",console="https://platform.kimi.ai/"},
    stepfun={name="StepFun",url="https://api.stepfun.com/v1/accounts",console="https://platform.stepfun.com/"},
    novita={name="Novita AI",url="https://api.novita.ai/openapi/v1/billing/balance/detail",currency="USD",console="https://novita.ai/console"},
    custom={name="Custom",kind="balance"},
}
function M.number(value)
    if type(value)~="number" and type(value)~="string" then return nil end
    if type(value)=="string" and (#value>40 or not value:match("^-?%d+%.?%d*$")) then return nil end
    local n=tonumber(value)
    if n and n==n and n~=math.huge and n~=-math.huge and math.abs(n)<=1e15 then return n end
end
-- A bounded field path, never Lua/eval: data.rows[0].remaining uses JSON indexes.
function M.path(root,path)
    if type(path)~="string" or #path==0 or #path>256 then return nil end
    local pos,value=1,root
    while pos<=#path do
        local key=path:sub(pos):match("^([%a_][%w_]*)")
        if key then
            if type(value)~="table" then return nil end
            value=value[key];pos=pos+#key
        elseif path:sub(pos,pos)=="[" then
            local index=path:sub(pos):match("^%[(%d+)%]")
            if not index or #index>6 or type(value)~="table" then return nil end
            value=value[tonumber(index)+1];pos=pos+#index+2
        else return nil end
        if pos<=#path then
            local c=path:sub(pos,pos)
            if c=="." then pos=pos+1;if pos>#path or not path:sub(pos):match("^[%a_]") then return nil end
            elseif c~="[" then return nil end
        end
    end
    return value
end
function M.safeUrl(url)
    if type(url)~="string" or #url>2048 or url:find("[%s%c\\#]") then return false end
    local authority=url:match("^https://([^/?]+)")
    return authority~=nil and not authority:find("@",1,true) and authority:match("^[%w%.%-]+:?%d*$")~=nil
end
local function metric(key,value)
    local n=M.number(value)
    if n then return {key=key,value=n} end
end
local function window(key,remaining,total,reset)
    remaining,total=M.number(remaining),M.number(total)
    if not remaining or not total or total<=0 or remaining<0 or remaining>total then return nil end
    return {key=key,remaining=remaining,total=total,percent=remaining/total*100,reset=type(reset)=="string" and reset:sub(1,64) or M.number(reset)}
end
local function balance(currency,remaining,metrics,available)
    local n=M.number(remaining)
    if not n then return nil end
    if available==nil then available=n>0 end
    return {kind="balance",currency=currency,remaining=n,metrics=metrics or {},available=available}
end
local function quota(windows,currency)
    if #windows==0 then return nil end
    local available=true
    for _,w in ipairs(windows) do if w.percent<=0 then available=false end end
    return {kind="quota",currency=currency,windows=windows,available=available}
end
function M.parse(id,payload,cfg)
    if type(payload)~="table" or payload.error~=nil then return nil end
    cfg=cfg or {};local p=M.presets[id];if not p then return nil end
    local d=payload.data
    if id=="deepseek" then
        if type(payload.is_available)~="boolean" or type(payload.balance_infos)~="table" then return nil end
        local chosen
        for _,item in ipairs(payload.balance_infos) do
            if type(item)~="table" or (item.currency~="CNY" and item.currency~="USD") or
                not M.number(item.total_balance) or not M.number(item.granted_balance) or not M.number(item.topped_up_balance) then return nil end
            if not chosen or item.currency==(cfg.currency or "CNY") then chosen=item end
        end
        if not chosen then return nil end
        return balance(chosen.currency,chosen.total_balance,{metric("topped",chosen.topped_up_balance),metric("granted",chosen.granted_balance)},payload.is_available)
    elseif id=="openrouter" then
        if type(d)~="table" then return nil end
        local total,used=M.number(d.total_credits),M.number(d.total_usage)
        if not total or not used or total<0 or used<0 then return nil end
        return balance("USD",total-used,{metric("purchased",total),metric("used",used)})
    elseif id=="openrouter_key" then
        if type(d)~="table" then return nil end
        local used=M.number(d.usage)
        if not used or used<0 then return nil end
        local total=M.number(d.limit)
        if d.limit==json.null then
            return {kind="quota",currency="USD",unlimited=true,available=true,metrics={metric("used",used)}}
        end
        if not total or total<0 then return nil end
        local remain=M.number(d.limit_remaining)
        if remain==nil then return nil end
        if total==0 then return {kind="quota",currency="USD",windows={{key="key_limit",remaining=0,total=0,percent=0}},available=false} end
        local w=window("key_limit",remain,total)
        return w and quota({w},"USD") or nil
    elseif id=="moonshot" or id=="moonshot_intl" then
        if type(d)~="table" or payload.status~=true or payload.code~=0 then return nil end
        local metrics={};for _,pair in ipairs({{"cash",d.cash_balance},{"voucher",d.voucher_balance}}) do
            local item=metric(pair[1],pair[2]);if item then metrics[#metrics+1]=item end end
        return balance(p.currency,d.available_balance,metrics)
    elseif id=="stepfun" then
        if payload.object~="account" then return nil end
        local metrics={}
        for _,pair in ipairs({{"topped",payload.total_cash_balance},{"granted",payload.total_voucher_balance}}) do
            local item=metric(pair[1],pair[2]);if item then metrics[#metrics+1]=item end
        end
        return balance(nil,payload.balance,metrics) -- The reference does not specify a currency; show raw units.
    elseif id=="novita" then
        local value=M.number(payload.availableBalance)
        local metrics={}
        for _,pair in ipairs({{"cash",payload.cashBalance},{"credit",payload.creditLimit}}) do
            local amount=M.number(pair[2]);if amount then metrics[#metrics+1]={key=pair[1],value=amount/10000} end
        end
        return value and balance(p.currency,value/10000,metrics) or nil
    elseif id=="custom" then
        local scale=M.number(cfg.scale) or 1
        if scale<=0 then return nil end
        local remain=M.number(M.path(payload,cfg.remainingPath))
        if not remain then return nil end
        if cfg.kind=="quota" then
            local total=M.number(M.path(payload,cfg.totalPath))
            local w=total and window("quota",remain*scale,total*scale)
            return w and quota({w}) or nil
        end
        local unit=(cfg.currency=="USD" or cfg.currency=="CNY") and cfg.currency or "CNY"
        return balance(unit,remain*scale)
    end
end
return M
