-- Bounded, incremental RFC 1950/1951 decoder; no filesystem or native helpers.
local M={}
local order={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15}
local lengths={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258}
local lengthExtra={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0}
local distances={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577}
local distanceExtra={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13}
local function huffman(sizes,allowEmpty)
    local counts,nextCode,codes={},{},{};local code,remaining=0,1;local maximum=0
    for _,size in ipairs(sizes) do
        assert(size>=0 and size<=15,"invalid Huffman size")
        if size>0 then counts[size]=(counts[size] or 0)+1;maximum=math.max(maximum,size) end
    end
    for size=1,15 do
        remaining=remaining*2-(counts[size] or 0);assert(remaining>=0,"oversubscribed Huffman tree")
        code=(code+(counts[size-1] or 0))*2;nextCode[size]=code
    end
    assert(maximum>0 or allowEmpty,"empty Huffman tree")
    for symbol,size in ipairs(sizes) do if size>0 then
        local value=nextCode[size];nextCode[size]=value+1;local reverse=0
        for _=1,size do reverse=(reverse<<1)|(value&1);value=value>>1 end
        codes[(1<<size)|reverse]=symbol-1
    end end
    return {codes=codes,maximum=maximum}
end
local function bits(s,count)
    while s.bitCount<count do
        local byte=s.input:byte(s.at);assert(byte,"truncated deflate stream")
        s.at=s.at+1;s.buffer=s.buffer|(byte<<s.bitCount);s.bitCount=s.bitCount+8
    end
    local result=s.buffer&((1<<count)-1);s.buffer=s.buffer>>count;s.bitCount=s.bitCount-count;return result
end
local function symbol(s,tree)
    local value=0
    for size=1,tree.maximum do
        value=value|(bits(s,1)<<(size-1));local decoded=tree.codes[(1<<size)|value]
        if decoded~=nil then return decoded end
    end
    error("invalid Huffman code")
end
local fixedLiteral,fixedDistance
local function fixed()
    if not fixedLiteral then
        local a,b={},{}
        for i=0,287 do a[i+1]=i<=143 and 8 or i<=255 and 9 or i<=279 and 7 or 8 end
        for i=1,32 do b[i]=5 end
        fixedLiteral,fixedDistance=huffman(a),huffman(b)
    end
    return fixedLiteral,fixedDistance
end
local function dynamic(s)
    local literalCount,distanceCount,codeCount=bits(s,5)+257,bits(s,5)+1,bits(s,4)+4
    assert(literalCount<=286,"reserved literal alphabet")
    local sizes={};for i=1,19 do sizes[i]=0 end
    for i=1,codeCount do sizes[order[i]+1]=bits(s,3) end
    local tree=huffman(sizes);local all={};local target=literalCount+distanceCount
    while #all<target do
        local v=symbol(s,tree)
        if v<=15 then all[#all+1]=v
        else
            local count,previous
            if v==16 then assert(#all>0,"repeat without previous code");count=bits(s,2)+3;previous=all[#all]
            elseif v==17 then count=bits(s,3)+3;previous=0
            elseif v==18 then count=bits(s,7)+11;previous=0
            else error("reserved code length") end
            assert(#all+count<=target,"code length overflow")
            for _=1,count do all[#all+1]=previous end
        end
    end
    assert(all[257]>0,"missing end of block")
    local literal,distance={},{}
    for i=1,literalCount do literal[i]=all[i] end
    for i=1,distanceCount do distance[i]=all[literalCount+i] end
    return huffman(literal),huffman(distance,true)
end
local function append(s,byte)
    assert(s.count<s.limit,"inflated lyrics exceed limit")
    s.count=s.count+1;s.window[(s.count-1)%32768+1]=byte;s.output[s.count]=string.char(byte)
    s.a=(s.a+byte)%65521;s.b=(s.b+s.a)%65521
end
function M.new(input,padding)
    assert(type(input)=="string" and #input>=6 and #input<=131072,"invalid zlib input")
    local a,b=input:byte(1,2)
    assert(a%16==8 and a>>4<=7 and (a*256+b)%31==0 and b&32==0,"unsupported zlib header")
    return {input=input,at=3,buffer=0,bitCount=0,output={},window={},count=0,a=1,b=0,limit=262144,padding=padding or 0}
end
function M.step(s)
    if s.done then return true,s.value end
    local budget=512
    while budget>0 do
        if s.copy and s.copy>0 then
            append(s,s.window[(s.count-s.distance)%32768+1]);s.copy=s.copy-1;budget=budget-1
        elseif s.stored and s.stored>0 then
            append(s,bits(s,8));s.stored=s.stored-1;budget=budget-1
        elseif not s.literal then
            if s.finishedBlock and s.final then
                s.buffer=0;s.bitCount=0
                local a,b,c,d=s.input:byte(s.at,s.at+3);assert(d,"missing zlib checksum")
                assert(((a<<24)|(b<<16)|(c<<8)|d)==((s.b<<16)|s.a),"zlib checksum mismatch")
                assert(#s.input-(s.at+3)<=s.padding,"unexpected compressed trailer")
                s.value=table.concat(s.output);s.done=true;s.output=nil;s.window=nil;return true,s.value
            end
            s.final=bits(s,1)==1;local kind=bits(s,2);s.finishedBlock=true
            if kind==0 then
                s.buffer=0;s.bitCount=0;local count,inverse=bits(s,16),bits(s,16)
                assert(count~inverse==65535,"stored block length mismatch");s.stored=count
                -- A non-nil marker distinguishes an empty stored block.
                s.literal=false
            elseif kind==1 then s.literal,s.distanceTree=fixed()
            elseif kind==2 then s.literal,s.distanceTree=dynamic(s)
            else error("reserved deflate block") end
            budget=budget-1
        else
            local v=symbol(s,s.literal)
            if v<256 then append(s,v);budget=budget-1
            elseif v==256 then s.literal=nil
            else
                local index=v-256;assert(lengths[index],"reserved length code")
                local count=lengths[index]+bits(s,lengthExtra[index]);local code=symbol(s,s.distanceTree)+1
                assert(distances[code],"reserved distance code")
                local distance=distances[code]+bits(s,distanceExtra[code])
                assert(distance<=s.count and distance<=32768,"invalid back reference")
                s.copy=count;s.distance=distance;budget=budget-1
            end
        end
    end
    return false
end
return M
