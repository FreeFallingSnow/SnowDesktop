-- Incremental QQ QRC / Kugou KRC transport decoding, independent of rendering.
local SP=module.require("modules/qrc_tables.lua")
local Inflate=module.require("modules/inflate.lua")
local M={}
local ip={58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
    57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7}
local fp={40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25}
local pc1={57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,
    63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4}
local pc2={14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
    41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32}
local shifts={1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1}
local krcKey={64,71,97,119,94,50,116,71,81,54,49,45,206,210,110,105}
local base64={};local alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
for i=1,#alphabet do base64[alphabet:byte(i)]=i-1 end
local function word(data,at)
    local a,b,c,d=data:byte(at,at+3);return a|(b<<8)|(c<<16)|(d<<24)
end
local function bytes(value)
    return string.char(value&255,(value>>8)&255,(value>>16)&255,(value>>24)&255)
end
local function permutation(value,positions)
    local result=0
    for _,position in ipairs(positions) do result=(result<<1)|((value>>(64-position))&1) end
    return result
end
local function schedule(key,reverse)
    local perm=permutation((word(key,1)<<32)|word(key,5),pc1)
    local a,b=(perm>>28)&0xfffffff,perm&0xfffffff;local result={}
    for round,shift in ipairs(shifts) do
        a=((a<<shift)|(a>>(28-shift)))&0xfffffff;b=((b<<shift)|(b>>(28-shift)))&0xfffffff
        local subkey=0
        for _,position in ipairs(pc2) do
            -- QRC's second half uses an extra one-bit shift relative to DES.
            local bit=position<=28 and ((a>>(28-position))&1) or ((b>>(55-position))&1)
            subkey=(subkey<<1)|bit
        end
        local groups={};for i=1,8 do groups[i]=(subkey>>(48-i*6))&63 end
        result[reverse and 17-round or round]=groups
    end
    return result
end
local schedules
local function keys()
    if not schedules then
        local key="!@#)(*$%123ZXC!@!@#)(NHL"
        schedules={schedule(key:sub(17,24),true),schedule(key:sub(9,16),false),schedule(key:sub(1,8),true)}
    end
    return schedules
end
local function decrypt(block)
    local value=permutation((word(block,1)<<32)|word(block,5),ip)
    local a,b=(value>>32)&0xffffffff,value&0xffffffff
    for _,rounds in ipairs(keys()) do
        for _,k in ipairs(rounds) do
            local f=SP[1][((((b&1)<<5)|(b>>27))~k[1])+1] ~ SP[2][(((b>>23)&63)~k[2])+1] ~
                SP[3][(((b>>19)&63)~k[3])+1] ~ SP[4][(((b>>15)&63)~k[4])+1] ~
                SP[5][(((b>>11)&63)~k[5])+1] ~ SP[6][(((b>>7)&63)~k[6])+1] ~
                SP[7][(((b>>3)&63)~k[7])+1] ~ SP[8][((((b&31)<<1)|(b>>31))~k[8])+1]
            a,b=b,(a~f)&0xffffffff
        end
        a,b=b,a
    end
    value=permutation((a<<32)|b,fp)
    return bytes((value>>32)&0xffffffff)..bytes(value&0xffffffff)
end
function M.new(kind,value)
    if type(value)~="string" or #value==0 or #value>174764 then return nil end
    if kind=="qrc" then
        value=value:gsub("^%s+",""):gsub("%s+$","")
        if #value%16~=0 or #value>131072 or value:find("[^%x]") then return nil end
    elseif kind=="krc" then
        value=value:gsub("%s","")
        if #value%4~=0 then return nil end
    else return nil end
    if #value==0 then return nil end
    return {kind=kind,input=value,at=1,output={},decoded=0}
end
local function advance(s)
    if s.inflate then
        local done,value=Inflate.step(s.inflate)
        if done then assert(utf8.len(value),"invalid lyric encoding");s.done=true;s.value=value;s.inflate=nil;return true,value end
        return false
    end
    if s.kind=="qrc" then
        for _=1,16 do
            if s.at>#s.input then break end
            local block={}
            for i=0,7 do block[i+1]=string.char(tonumber(s.input:sub(s.at+i*2,s.at+i*2+1),16)) end
            s.output[#s.output+1]=decrypt(table.concat(block));s.at=s.at+16
        end
    else
        for _=1,128 do
            if s.at>#s.input then break end
            local a,b,c,d=s.input:byte(s.at,s.at+3);s.at=s.at+4
            local x,y,z,w=base64[a],base64[b],base64[c],base64[d]
            assert(x and y and (z or c==61) and (w or d==61),"invalid KRC base64")
            assert(c~=61 or d==61,"invalid base64 padding")
            assert((c~=61 and d~=61) or s.at>#s.input,"base64 padding before end")
            local group=(x<<18)|(y<<12)|((z or 0)<<6)|(w or 0)
            for i=1,(c==61 and 1 or d==61 and 2 or 3) do
                local byte=(group>>(24-i*8))&255;s.decoded=s.decoded+1
                if s.decoded<=4 then assert(byte==("krc1"):byte(s.decoded),"missing KRC header")
                else s.output[#s.output+1]=string.char(byte~krcKey[(s.decoded-5)%16+1]) end
            end
        end
    end
    if s.at>#s.input then
        s.inflate=Inflate.new(table.concat(s.output),s.kind=="qrc" and 8 or 0);s.output=nil;s.input=nil
    end
    return false
end
function M.step(s)
    if not s or s.error then return true,nil end
    if s.done then return true,s.value end
    local ok,done,value=pcall(advance,s)
    if not ok then s.error=done;s.input=nil;s.output=nil;s.inflate=nil;return true,nil end
    return done,value
end
return M
