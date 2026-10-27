local M={}
function M.theme()
    local darkText=widget.theme().contentTheme==1
    return darkText and {primary=0x18212C,secondary=0x667180,surface=0xE8EDF3,well=0xD9E1EB,
        accent=0x0068D9,selected=0xD5E6FA,onAccent=0xFFFFFF} or
        {primary=0xF6F8FC,secondary=0x9CAABC,surface=0x202A37,well=0x354253,
        accent=0x80B9FF,selected=0x253F5D,onAccent=0x12243C}
end
function M.text(key,text,row,options)
    local spec={key=key,text=text,width="fill",height=row*0.7,fontSize=row*0.44,
        textWrap="noWrap",overflowText="ellipsis",style={foreground=M.theme().primary}}
    for k,v in pairs(options or {}) do spec[k]=v end
    return view.text(spec)
end
function M.icon(key,glyph,row,color)
    return view.icon({key=key,glyph=utf8.char(glyph),iconFont="fa",width=row,height=row,
        fontSize=row*0.45,style={foreground=color or M.theme().primary}})
end
function M.iconButton(key,glyph,label,row,action,enabled,active)
    local c=M.theme()
    return view.iconButton({key=key,glyph=utf8.char(glyph),iconFont="fa",width=row,height=row,
        fontSize=row*0.44,enabled=enabled~=false,
        style={foreground=active and c.onAccent or c.secondary,background=active and c.accent or c.surface,
            cornerRadius=row*0.5,borderWidth=0},events={click={id=action}},accessibility={label=label}})
end
function M.choice(key,label,row,action,selected,enabled,value)
    local c=M.theme()
    return view.button({key=key,label=label,width="fill",height=row*1.12,fontSize=row*0.43,
        padding={horizontal=row*0.25},textAlign="center",enabled=enabled~=false,
        style={foreground=selected and c.accent or c.primary,background=selected and c.selected or c.surface,
            cornerRadius=row*0.35,borderWidth=0},events={click={id=action,value=value}},accessibility={label=label}})
end
function M.well(key,row,children)
    return view.column({key=key,width="fill",height="auto",padding=row*0.5,gap=row*0.25,
        style={background=M.theme().surface,cornerRadius=row*0.55},children=children})
end
function M.volume(key,label,scalar,row,change,commit,leading,enabled)
    local c=M.theme()
    local value=scalar~=nil and math.floor(scalar*100+0.5).."%" or "—"
    return M.well(key..".well",row,{
        view.row({key=key..".heading",width="fill",height=row*0.85,alignItems="center",children={
            M.text(key..".label",label,row,{style={foreground=c.secondary}}),
            M.text(key..".value",value,row,{width=row*2.6,minWidth=row*2.6,height=row*0.85,fontSize=row*0.66,
                fontWeight=600,flexShrink=0,textAlign="end",overflowText="clip"})}}),
        view.row({key=key..".controls",width="fill",height=row*1.35,gap=row*0.35,alignItems="center",children={
            leading or M.icon(key..".speaker",0xF028,row,c.secondary),
            view.slider({key=key,width="fill",height=row*1.35,min=0,max=1,step=0.01,value=scalar or 0,
                enabled=scalar~=nil and enabled~=false,style={foreground=c.primary,background=c.well,borderWidth=0},
                events={change={id=change},pointerUp={id=commit}},accessibility={label=label}})}})
    })
end
return M
