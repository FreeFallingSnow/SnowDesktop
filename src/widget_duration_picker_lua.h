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
    bad={};committed=n
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
    if values[id]==nil and id~="confirm" then return nil end
    local result={handled=true,changed=false}
    if disabled or readOnly then return result end
    if id=="confirm" then commit(result);return result end
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
        fields[#fields+1]=view.column({key=key..id..".field",width="fill",height=r*1.9,flexShrink=0,gap=r*0.15,children={
            view.text({key=key..id..".label",text=labels[id],width="fill",height=r*0.75,fontSize=r*0.43,
                textAlign="center",verticalAlign="center",style={foreground="textSecondary"}}),
            view.numberInput({key=key..id,value=values[id],min=0,max=id=="hours" and 99 or 59,step=1,
                width="fill",height=r,fontSize=r*0.6,textAlign="center",padding={horizontal=r*0.2,vertical=0},
                style={foreground="textPrimary"},enabled=not disabled,readOnly=readOnly,
                validationState=bad[id] and "error" or "none",validationMessage=bad[id] and labels.invalid or "",
                events={change={id=key..id},submit={id=key.."confirm"}},accessibility={label=labels[id]}})}})
    end
    local children={view.row({key=key.."fields",width="fill",height=r*1.9,flexShrink=0,gap=r*0.4,children=fields})}
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
