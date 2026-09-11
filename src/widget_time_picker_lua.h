#pragma once
inline constexpr char kWidgetTimePickerLua[] = R"SNOWLUA(
local o,labels=...
assert(type(o)=="table" and type(o.key)=="string" and #o.key>0 and #o.key<=80,"timePicker requires a stable key")
assert(o.mode==nil or o.mode=="single" or o.mode=="range","invalid time picker mode")
local range=o.mode=="range"
local step=o.minuteStep or 1
assert(type(step)=="number" and step>=1 and step<=30 and step%1==0 and 60%step==0,"invalid minuteStep")
local function parse(s)
 if type(s)~="string" then return end
 local h,m=s:match("^(%d%d):(%d%d)$");h,m=tonumber(h),tonumber(m)
 if h and h<24 and m<60 then return h*60+m end
end
local low,high=parse(o.minTime or "00:00"),parse(o.maxTime or "23:59")
assert(low and high and low<=high,"invalid time bounds")
local function allowed(s) local n=parse(s);return n and n>=low and n<=high and n%step==0 end
local key=o.key..":"
local self={startTime="",endTime="",opened={}}
local function output() return range and {startTime=self.startTime,endTime=self.endTime} or self.startTime end
local function copy(v) return type(v)=="table" and {startTime=v.startTime,endTime=v.endTime} or v end
local committed=range and {startTime="",endTime=""} or ""
function self:value() return copy(committed) end
function self:draftValue() return output() end
function self:validation()
 if self.startTime=="" and (not range or self.endTime=="") and o.allowClear~=false then return end
 if not allowed(self.startTime) or (range and not allowed(self.endTime)) then return labels.invalid end
 if range and self.endTime<self.startTime then return labels.order end
end
function self:setValue(v)
 local a,b=self.startTime,self.endTime
 if range then assert(type(v)=="table","range value requires startTime/endTime");self.startTime,self.endTime=v.startTime or "",v.endTime or ""
 else self.startTime,self.endTime=v or "","" end
 local err=self:validation()
 if err then self.startTime,self.endTime=a,b;return false,err end
 committed=output();return true
end
local initial=o.value or (range and {startTime="",endTime=""} or "")
if not self:setValue(initial) then
 self.startTime=type(initial)=="table" and (initial.startTime or "") or initial
 self.endTime=type(initial)=="table" and (initial.endTime or "") or ""
end
function self:handle(e)
 if e.kind~="action" or type(e.id)~="string" or e.id:sub(1,#key)~=key then return end
 local result={handled=true,changed=false}
 if o.disabled then return result end
 local id=e.id:sub(#key+1)
 if id=="clear" and o.allowClear~=false then self.startTime="";self.endTime=""
 elseif id=="startTime" or (range and id=="endTime") then
  if type(e.text)=="string" and #e.text<=5 then self[id]=e.text end
 else
  local field,part=id:match("^(%a+):([hm])$")
  if field=="startTime" or (range and field=="endTime") then
   if e.expanded~=nil then self.opened[id]=e.expanded==true end
   local n=tonumber(e.selection)
   if n and n%1==0 and n>=0 and ((part=="h" and n<24) or (part=="m" and n<60 and n%step==0)) then
    local old=parse(self[field]) or 0
    local h,m=math.floor(old/60),old%60
    if part=="h" then h=n else m=n end
    self[field]=string.format("%02d:%02d",h,m);self.opened[id]=false
   end
  end
 end
 if (id=="confirm" or o.needConfirm==false) and not self:validation() then
  committed=output();result.changed=true;result.value=copy(committed)
 end
 return result
end
function self:view(options)
 local r=options and options.rowHeight or ui.metrics().layoutRowHeight
 local children={}
 local function button(id,label,enabled) return view.button({key=key..id,label=label,width="fill",height=r,
  fontSize=r*.46,textAlign="center",enabled=not o.disabled and enabled~=false,action={id=key..id},
  style={foreground="textPrimary",cornerRadius=r*.12}}) end
 local function field(name,label)
  children[#children+1]=view.text({key=key..name..".label",text=label,height=r,fontSize=r*.46,style={foreground="textSecondary"}})
  local invalid=not allowed(self[name])
  children[#children+1]=view.textInput({key=key..name,value=self[name],height=r,fontSize=r*.46,maxBytes=5,
   enabled=not o.disabled,action={id=key..name},style={foreground="textPrimary"},
   validationState=invalid and "error" or "none",validationMessage=invalid and labels.invalid or "",accessibility={label=label}})
  local n=parse(self[name]);local selectors={}
  for _,part in ipairs({"h","m"}) do
   local opts={};for i=0,(part=="h" and 23 or 59),(part=="h" and 1 or step) do
    opts[#opts+1]={key=tostring(i),value=tostring(i),label=string.format("%02d",i)}
   end
   local id=name..":"..part
   selectors[#selectors+1]=view.select({key=key..id,options=opts,selectedValue=n and tostring(part=="h" and math.floor(n/60) or n%60) or "",
    placeholder=part=="h" and labels.hour or labels.minute,width="fill",height=r,fontSize=r*.46,textAlign="center",
    enabled=not o.disabled,expanded=self.opened[id]==true,events={click={id=key..id},change={id=key..id}},
    style={foreground="textPrimary"},accessibility={label=label.." "..(part=="h" and labels.hour or labels.minute)}})
  end
  children[#children+1]=view.row({key=key..name..".select",height=r,gap=r*.25,children=selectors})
 end
 field("startTime",range and labels.startTime or labels.time)
 if range then field("endTime",labels.endTime) end
 local err=self:validation()
 if err then children[#children+1]=view.text({key=key.."error",text=err,height=r,fontSize=r*.42,style={foreground="textSecondary"}}) end
 local footer={}
 if o.allowClear~=false then footer[#footer+1]=button("clear",labels.clear) end
 if o.needConfirm~=false then footer[#footer+1]=button("confirm",labels.confirm,not err) end
 if #footer>0 then children[#children+1]=view.row({key=key.."footer",height=r,gap=r*.25,children=footer}) end
 return view.column({key=key.."root",width="fill",height="auto",gap=r*.25,children=children})
end
return self
)SNOWLUA";
