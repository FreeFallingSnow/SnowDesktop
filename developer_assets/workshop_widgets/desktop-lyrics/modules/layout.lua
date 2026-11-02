local M={}
function M.metrics(width,height,row,scale,spacing,subtitle)
    local font=row*0.78*scale/100
    local step=font*(spacing/100)+(subtitle and font*0.70 or 0)
    local padding=row*0.35
    local count=math.max(1,math.floor((height-2*padding)/step))
    return {font=font,step=step,count=count,gap=font*math.max(0.05,spacing/100-1.35),padding=padding,width=math.max(1,width-2*padding)}
end
function M.focus(previous,target,elapsed,snap)
    if not previous or snap then return target end
    return target+(previous-target)*math.exp(-math.max(0,elapsed)/105)
end
function M.opacity(distance,count)
    local edge=math.max(1,count/2+0.25)
    local fade=math.max(0,1-math.abs(distance)/edge)
    return fade*fade
end
return M
