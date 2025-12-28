if false then
-- ssrc_dec.lua: add RTP SSRC as an explicit decimal field for use in a custom column

local p = Proto("ssrc_dec", "SSRC Decimal")

-- Create a new field that displays in decimal
local pf_ssrc_dec = ProtoField.uint32("ssrc_dec.ssrc", "SSRC (dec)", base.DEC)
p.fields = { pf_ssrc_dec }

-- Field extractor for the existing RTP SSRC
local f_rtp_ssrc = Field.new("rtp.ssrc")

function p.dissector(tvb, pinfo, tree)
    -- Only do anything if RTP SSRC exists in this packet
    local fi = f_rtp_ssrc()
    if not fi then return end

    -- Add our own little subtree to the packet details pane
    local st = tree:add(p, "SSRC Decimal")
    st:add(pf_ssrc_dec, fi.value)
end

register_postdissector(p)
end


-- prepend_ssrc_dec_info.lua

local f_rtp_ssrc = Field.new("rtp.ssrc")

local p = Proto("ssrc_info", "SSRC Info Prepend")

function p.dissector(tvb, pinfo, tree)
    local fi = f_rtp_ssrc()
    if not fi then return end

    -- Robust convert to number (handles fi being a FieldInfo)
    local v = tonumber(tostring(fi))
    if not v then return end

    -- Prepend only once (Wireshark may dissect multiple passes)
    local cur = tostring(pinfo.cols.info)
    if string.find(cur, "^SSRC=%d+ ") then return end

    pinfo.cols.info = string.format("SSRC=%u %s", v, cur)
end

register_postdissector(p)
