#pragma once

// Host-owned composition; the same controller runs in the sandbox and tests.
inline constexpr char kWidgetDurationPickerLua[] = R"SNOWLUA(
local o,labels=...
assert(type(o)=="table" and type(o.key)=="string" and #o.key>0 and #o.key<=80,
    "ui.durationPicker requires a stable key (1-80 bytes)")
local function integer(n) return type(n)=="number" and n==n and n>=0 and n<=359999 and n%1==0 end
local low,high=o.minSeconds or 0,o.maxSeconds or 359999
assert(integer(low) and integer(high) and low<=high,"invalid duration bounds")
for _,name in ipairs({"disabled","readOnly","needConfirm"}) do
    assert(o[name]==nil or type(o[name])=="boolean","invalid duration option: "..name)
end
local disabled,readOnly,needConfirm=o.disabled==true,o.readOnly==true,o.needConfirm~=false
local key=o.key..":"
local values={hours=0,minutes=0,seconds=0}
local bad={}
local wheelRemainder={}
local committed
local self={}
local function allowed(n) return integer(n) and n>=low and n<=high end
function self:draftValue()
    if next(bad) then return nil end
    return values.hours*3600+values.minutes*60+values.seconds
end
function self:validation()
    if not allowed(self:draftValue()) then return labels.invalid end
end
function self:value() return committed end
function self:setValue(n)
    if not allowed(n) then return false,labels.invalid end
    values.hours=math.floor(n/3600)
    values.minutes=math.floor(n/60)%60
    values.seconds=n%60
    bad={};wheelRemainder={};committed=n
    return true
end
assert(self:setValue(o.value==nil and low or o.value),"invalid initial duration")
local function commit(result)
    if self:validation() then return end
    local n=self:draftValue()
    result.changed=n~=committed
    result.value=n
    committed=n
end
function self:handle(e)
    if type(e)~="table" or e.kind~="action" or type(e.id)~="string" or e.id:sub(1,#key)~=key then return nil end
    local id=e.id:sub(#key+1)
    local field,operation=id:match("^(%a+)%.(%a+)$")
    local stepAction=values[field]~=nil and (operation=="up" or operation=="down" or operation=="wheel")
    if values[id]==nil and id~="confirm" and not stepAction then return nil end
    local result={handled=true,changed=false}
    if disabled or readOnly then return result end
    if id=="confirm" then commit(result);return result end
    if stepAction then
        local step=operation=="up" and 1 or -1
        if operation=="wheel" then
            local delta=e.delta
            if type(delta)~="number" or delta~=delta or math.abs(delta)>12000 or delta%1~=0 then return result end
            local accumulated=(wheelRemainder[field] or 0)+delta
            step=accumulated>=0 and math.floor(accumulated/120) or math.ceil(accumulated/120)
            wheelRemainder[field]=accumulated-step*120
        end
        if step~=0 then
            values[field]=math.max(0,math.min(field=="hours" and 99 or 59,values[field]+step))
            bad[field]=nil
            if not needConfirm then commit(result) end
        end
        return result
    end
    local n=e.controlValue
    if e.numberValid==true and integer(n) and n<=(id=="hours" and 99 or 59) then
        values[id]=n;bad[id]=nil
    else bad[id]=true end
    if not needConfirm then commit(result) end
    return result
end
function self:view(options)
    local r=options and options.rowHeight or ui.metrics().layoutRowHeight
    assert(type(r)=="number" and r==r and r>0 and r<=512,"invalid duration rowHeight")
    local fields={}
    for _,id in ipairs({"hours","minutes","seconds"}) do
        local function stepper(direction,caption)
            return view.button({key=key..id.."."..direction,label=caption,width=r,height=r,flexShrink=0,
                padding=0,fontSize=r*0.6,textAlign="center",enabled=not disabled and not readOnly,
                action={id=key..id.."."..direction},
                accessibility={label=(direction=="up" and labels.increase or labels.decrease).." "..labels[id]},
                style={foreground="textPrimary",cornerRadius=r*0.15},hoverStyle={opacity=0.75},pressedStyle={opacity=0.55}})
        end
        fields[#fields+1]=view.row({key=key..id..".field",width="fill",height=r,flexShrink=0,gap=r*0.5,alignItems="center",children={
            view.text({key=key..id..".label",text=labels[id],width=r*2.4,height=r,flexShrink=0,fontSize=r*0.46,
                textAlign="start",verticalAlign="center",style={foreground="textSecondary"}}),
            view.numberInput({key=key..id,value=values[id],min=0,max=id=="hours" and 99 or 59,step=1,
                width="fill",height=r,fontSize=r*0.6,textAlign="center",padding={horizontal=r*0.2,vertical=0},
                style={foreground="textPrimary"},enabled=not disabled,readOnly=readOnly,
                validationState=bad[id] and "error" or "none",validationMessage=bad[id] and labels.invalid or "",
                events={change={id=key..id},submit={id=key.."confirm"},wheel={id=key..id..".wheel"}},accessibility={label=labels[id]}}),
            view.row({key=key..id..".steppers",width=r*2.2,height=r,flexShrink=0,gap=r*0.2,
                children={stepper("down","−"),stepper("up","+")}})}})
    end
    local children={view.column({key=key.."fields",width="fill",height=r*3.9,flexShrink=0,gap=r*0.45,children=fields})}
    local err=self:validation()
    if err then children[#children+1]=view.text({key=key.."error",text=err,width="fill",height=r*1.5,
        fontSize=r*0.43,textWrap="wrap",style={foreground="textPrimary"}}) end
    if needConfirm then children[#children+1]=view.button({key=key.."confirm",label=labels.confirm,width="fill",height=r,
        fontSize=r*0.46,textAlign="center",padding={vertical=0,horizontal=r*0.2},
        enabled=not disabled and not readOnly and not err,action={id=key.."confirm"},accessibility={label=labels.confirm},
        style={background=0x438BF5,foreground=0xFFFFFF,cornerRadius=r*0.2}}) end
    return view.column({key=key.."root",width="fill",height="auto",gap=r*0.35,children=children})
end
return self
)SNOWLUA";
