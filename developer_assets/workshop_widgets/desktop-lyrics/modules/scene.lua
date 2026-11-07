local M={}
local function intersects(a,b)return a.x<a.x+a.w and a.y<a.y+a.h and a.x<b.x+b.w and a.x+a.w>b.x and a.y<b.y+b.h and a.y+a.h>b.y end
function M.new(width,height,label)
    local root={type="stack",key="lyrics",width=width,height=height,overflow="clip",children={},
        events={contextMenu={id="lyrics.menu",scope="component"}},accessibility={role="text",label=label}}
    local frames={{node=root,x=0,y=0,w=width,h=height}};local serial=0;local metadata={}
    local paint={}
    local function key()serial=serial+1;return "lyrics.draw."..serial end
    function paint.text(x,y,value,font,color,boxWidth,bold,_,boxHeight,alpha)
        local parent=frames[#frames];local bounds={x=x,y=y,w=boxWidth,h=boxHeight}
        if not parent.node or not intersects(bounds,parent) then return end
        local node={type="text",key=key(),text=value,width=math.max(1,boxWidth),height=math.max(1,boxHeight),
            offset={x=x-parent.x,y=y-parent.y},fontSize=font,fontWeight=bold and 700 or 400,
            textAlign="start",verticalAlign="start",textWrap="noWrap",maxLines=1,overflowText="clip",
            style={foreground=color,opacity=alpha or 1}}
        if color==0xFFFFFF or color==0x142235 then node.style.foreground="textPrimary" end
        parent.node.children[#parent.node.children+1]=node
        metadata[node]={x=x,y=y,text=value,font=font,color=color,width=boxWidth,height=boxHeight,alpha=alpha,bold=bold}
    end
    function paint.pushClip(x,y,w,h)
        local parent=frames[#frames];local frame={x=x,y=y,w=w,h=h}
        if parent.node and w>0 and h>0 and intersects(frame,parent) then
            local node={type="stack",key=key(),width=w,height=h,offset={x=x-parent.x,y=y-parent.y},overflow="clip",children={}}
            parent.node.children[#parent.node.children+1]=node;frame.node=node
        end
        frames[#frames+1]=frame
    end
    function paint.popClip()
        local current=table.remove(frames);local parent=frames[#frames]
        if not current.node or not parent then return end
        local list=parent.node.children;local node=current.node
        if #node.children==0 then table.remove(list);return end
        local previous=list[#list-1]
        if #node.children==1 and previous and previous.type=="stack" and #previous.children==1 then
            local a,b=metadata[previous.children[1]],metadata[node.children[1]]
            if a and b and a.x==b.x and a.y==b.y and a.text==b.text and a.font==b.font and a.color==b.color and
                a.width==b.width and a.height==b.height and a.alpha==b.alpha and a.bold==b.bold and
                previous.offset.y==node.offset.y and previous.height==node.height and
                node.offset.x<=previous.offset.x+previous.width+a.font/2 and node.offset.x>=previous.offset.x then
                previous.width=math.max(previous.width,node.offset.x+node.width-previous.offset.x)
                table.remove(list)
            end
        end
    end
    function paint.add(node)root.children[#root.children+1]=node end
    function paint.finish()return root end
    return paint
end
return M
