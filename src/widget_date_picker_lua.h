#pragma once

// Host-owned composition of existing declarative controls. Kept as Lua so the
// same state machine can run in the sandbox and in behavioral tests.
inline constexpr char kWidgetDatePickerLua[] = R"SNOWLUA(
local o, labels = ...
assert(type(o) == "table" and type(o.key) == "string" and #o.key > 0 and #o.key <= 80,
    "ui.datePicker requires a stable key (1-80 bytes)")
local range = o.mode == "range"
assert(o.mode == nil or o.mode == "single" or range, "invalid date picker mode")
local function leap(y) return y % 4 == 0 and (y % 100 ~= 0 or y % 400 == 0) end
local function days(y,m)
    if m == 2 then return leap(y) and 29 or 28 end
    return ({31,28,31,30,31,30,31,31,30,31,30,31})[m]
end
local function parts(s)
    if type(s) ~= "string" then return end
    local y,m,d = s:match("^(%d%d%d%d)%-(%d%d)%-(%d%d)$")
    y,m,d = tonumber(y),tonumber(m),tonumber(d)
    if y and y >= 1 and y <= 9999 and m >= 1 and m <= 12 and d >= 1 and d <= days(y,m) then
        return y,m,d
    end
end
local function iso(y,m,d) return string.format("%04d-%02d-%02d",y,m,d) end
local function serial(y,m,d)
    local prior=y-1
    local n=prior*365+math.floor(prior/4)-math.floor(prior/100)+math.floor(prior/400)+d
    for i=1,m-1 do n=n+days(y,i) end
    return n
end
assert(parts(o.todayDate), "todayDate must be a valid ISO date")
local low, high = o.minDate or "0001-01-01", o.maxDate or "9999-12-31"
assert(parts(low) and parts(high) and low <= high, "invalid date picker bounds")
local disabled = {}
assert(o.disabledDates == nil or type(o.disabledDates) == "table", "disabledDates must be an array")
assert(#(o.disabledDates or {}) <= 366, "too many disabled dates")
for _,s in ipairs(o.disabledDates or {}) do assert(parts(s), "invalid disabled date"); disabled[s]=true end
local function allowed(s) return parts(s) ~= nil and s >= low and s <= high and not disabled[s] end
local firstDay = o.firstDayOfWeek or 1
assert(firstDay >= 1 and firstDay <= 7 and firstDay == math.floor(firstDay), "invalid firstDayOfWeek")
local key=o.key..":"
local self={startDate="",endDate="",year=1,month=1}
local committed
local function output()
    return range and {startDate=self.startDate,endDate=self.endDate} or self.startDate
end
local function copy(v) return type(v)=="table" and {startDate=v.startDate,endDate=v.endDate} or v end
function self:validation()
    if self.startDate=="" and (not range or self.endDate=="") and o.allowClear ~= false then return nil end
    if not allowed(self.startDate) or (range and not allowed(self.endDate)) then return labels.invalid end
    if range then
        if self.endDate < self.startDate then return labels.order end
        for s in pairs(disabled) do
            if s >= self.startDate and s <= self.endDate then return labels.invalid end
        end
    end
end
function self:value() return copy(committed) end
function self:draftValue() return output() end
function self:setValue(v)
    local a,b=self.startDate,self.endDate
    if range then
        assert(type(v)=="table", "range value requires startDate/endDate")
        self.startDate,self.endDate=v.startDate or "",v.endDate or ""
    else self.startDate,self.endDate=v or "","" end
    local err=self:validation()
    if err then self.startDate,self.endDate=a,b; return false,err end
    committed=output()
    self.year,self.month=parts(self.startDate ~= "" and self.startDate or o.todayDate)
    return true
end
local initial=o.value or (range and {startDate="",endDate=""} or "")
-- A required picker may start empty; that is an invalid draft, not a fabricated date.
if not self:setValue(initial) then
    self.startDate=type(initial)=="table" and (initial.startDate or "") or initial
    self.endDate=type(initial)=="table" and (initial.endDate or "") or ""
    committed=range and {startDate="",endDate=""} or ""
    self.year,self.month=parts(o.todayDate)
end
local function commit(result)
    if self:validation() then return end
    committed=output(); result.changed=true; result.value=copy(committed)
end
function self:handle(e)
    if e.kind~="action" or type(e.id)~="string" or e.id:sub(1,#key)~=key then return nil end
    local result={handled=true,changed=false}
    if o.disabled then return result end
    local id=e.id:sub(#key+1)
    if id=="start" or id=="end" then
        if type(e.text)=="string" and #e.text<=10 then self[id=="start" and "startDate" or "endDate"]=e.text end
    elseif id=="year" then
        if e.numberValid and e.controlValue and e.controlValue%1==0 then self.year=e.controlValue end
    elseif id=="month.open" then self.monthOpen=e.expanded==true
    elseif id=="month" then
        local m=tonumber(e.selection)
        if m and m>=1 and m<=12 then self.month=m;self.monthOpen=false end
    elseif id=="previous" or id=="next" then
        local n=(self.year-1)*12+self.month-1+(id=="next" and 1 or -1)
        if n>=0 and n<9999*12 then self.year=math.floor(n/12)+1; self.month=n%12+1 end
    elseif id=="today" then self.year,self.month=parts(o.todayDate)
    elseif id=="clear" and o.allowClear~=false then
        self.startDate,self.endDate="",""
        if o.needConfirm==false then commit(result) end
    elseif id=="confirm" then commit(result)
    elseif id:sub(1,4)=="day:" then
        local s=id:sub(5)
        if allowed(s) then
            if range and self.startDate~="" and self.endDate=="" and allowed(self.startDate) then
                self.endDate=s
                if s<self.startDate then self.startDate,self.endDate=s,self.startDate end
            else self.startDate=s; self.endDate="" end
            if o.needConfirm==false then commit(result) end
        end
    end
    if (id=="start" or id=="end") and o.needConfirm==false then commit(result) end
    self.year=math.max(1,math.min(9999,self.year))
    return result
end
function self:view(options)
    local r=options and options.rowHeight or ui.metrics().layoutRowHeight
    local font=r*0.46
    local function text(id,s) return view.text({key=key..id,text=s,height=r,fontSize=font,
        textAlign=id=="error" and "start" or "center",
        verticalAlign="center",style={foreground="textSecondary"}}) end
    local function button(id,s,enabled,selected)
        return view.button({key=key..id,label=s,width="fill",height=r,fontSize=font,textAlign="center",
            enabled=not o.disabled and enabled~=false,action={id=key..id},
            style={foreground=selected and 0xFFFFFF or "textPrimary",
                background=selected and 0x175CD3 or nil,cornerRadius=r*0.12},accessibility={label=s}})
    end
    local function input(id,value,label)
        local bad=value~="" and not allowed(value)
        return view.textInput({key=key..id,value=value,placeholder=label,width="fill",height=r,
            fontSize=font,maxBytes=10,style={foreground="textPrimary"},enabled=not o.disabled,action={id=key..id},
            validationState=bad and "error" or "none",validationMessage=bad and labels.invalid or "",
            accessibility={label=label}})
    end
    local fields={input("start",self.startDate,range and labels.startDate or labels.date)}
    if range then fields[#fields+1]=input("end",self.endDate,labels.endDate) end
    local months={}
    for m=1,12 do months[m]={key=tostring(m),value=tostring(m),label=tostring(m)} end
    local nav={button("previous",labels.previous,self.year>1 or self.month>1),
        view.numberInput({key=key.."year",value=self.year,min=1,max=9999,step=1,
            width="fill",height=r,fontSize=font,textAlign="center",style={foreground="textPrimary"},enabled=not o.disabled,action={id=key.."year"},
            accessibility={label=labels.year}}),
        view.select({key=key.."month",selectedValue=tostring(self.month),options=months,width="fill",height=r,
            fontSize=font,textAlign="center",style={foreground="textPrimary"},enabled=not o.disabled,expanded=self.monthOpen==true,
            events={change={id=key.."month"},click={id=key.."month.open"}},accessibility={label=labels.month}}),
        button("next",labels.next,self.year<9999 or self.month<12)}
    local cells={}
    for i=0,6 do cells[#cells+1]=text("weekday"..i,labels["weekday"..((firstDay-1+i)%7+1)]) end
    -- 0001-01-01 is Monday; API weekdays start with Sunday.
    local lead=(serial(self.year,self.month,1)%7+1-firstDay+7)%7
    for i=1,42 do
        local d=i-lead
        if d<1 or d>days(self.year,self.month) then cells[#cells+1]=text("blank"..i,"")
        else
            local s=iso(self.year,self.month,d)
            local selected=s==self.startDate or (range and self.endDate~="" and s>=self.startDate and s<=self.endDate)
            local cell=button("day:"..s,tostring(d),allowed(s),selected)
            cell.accessibility.label=s
            cells[#cells+1]=cell
        end
    end
    local children={view.row({key=key.."inputs",height=r,gap=r*0.25,children=fields}),
        view.row({key=key.."nav",height=r,gap=r*0.25,children=nav}),
        view.grid({key=key.."grid",columns=7,height=r*7+r*0.12*6,gap=r*0.12,children=cells})}
    local err=self:validation()
    if err then children[#children+1]=text("error",err) end
    local footer={button("today",labels.today)}
    if o.allowClear~=false then footer[#footer+1]=button("clear",labels.clear) end
    if o.needConfirm~=false then footer[#footer+1]=button("confirm",labels.confirm,not err) end
    children[#children+1]=view.row({key=key.."footer",height=r,gap=r*0.25,children=footer})
    return view.column({key=key.."root",width="fill",height="auto",gap=r*0.25,children=children})
end
return self
)SNOWLUA";
