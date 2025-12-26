-- ka9q_ctl.lua - Wireshark Lua dissector for ka9q-radio control/status protocol
-- UDP port: 5006
--
-- Wire format:
--   u8 msg_class  (0=STATUS, 1=CMD)
--   repeated TLVs:
--     u8  tlv_type
--     len (BER-style):
--        if first_len < 0x80: length = first_len
--        else: n = first_len & 0x7f; next n bytes big-endian length
--     value[length]
--
-- TLV type 0 (EOL) terminates parsing immediately. Extra bytes after EOL are flagged.
--
-- Unsigned integers are variable-length big-endian; length 0 means 0.
-- Floats/doubles are sender "machine order" (endianness is a preference).
-- Socket values: length 6 (IPv4), 10 (compact-IPv6: 8 bytes), or 18 (full IPv6: 16 bytes),
--                followed by 2-byte port in network order.

local ka9q = Proto("ka9qctl", "ka9q-radio Control/Status")

-- Preferences
ka9q.prefs.float_little_endian = Pref.bool("Float/Dbl are little-endian (common on x86/ARM)", false)
ka9q.prefs.add_info_column     = Pref.bool("Populate Info column (msg class + key TLVs)", true)
ka9q.prefs.show_raw_packet     = Pref.bool("Show raw packet bytes (debug)", false)

-- Fields
local f = ka9q.fields
f.msg_class = ProtoField.uint8("ka9qctl.class", "Packet Type", base.DEC, { [0]="STATUS", [1]="CMD" })
f.raw_packet = ProtoField.bytes("ka9qctl.raw", "Raw Packet Data")

f.tlv_type  = ProtoField.uint8("ka9qctl.tlv.type", "TLV Type", base.DEC)
f.tlv_len   = ProtoField.uint32("ka9qctl.tlv.len", "TLV Length", base.DEC)
f.tlv_raw   = ProtoField.bytes("ka9qctl.tlv.raw", "TLV Value (raw)")

f.uint      = ProtoField.uint64("ka9qctl.uint", "Unsigned Integer", base.DEC)
f.float     = ProtoField.float("ka9qctl.float", "Float32")
f.double    = ProtoField.double("ka9qctl.double", "Float64")
f.str       = ProtoField.string("ka9qctl.string", "String")

f.sock_addr4 = ProtoField.ipv4("ka9qctl.sock.addr4", "Socket IPv4 Address")
f.sock_addr6 = ProtoField.ipv6("ka9qctl.sock.addr6", "Socket IPv6 Address")
f.sock_port = ProtoField.uint16("ka9qctl.sock.port", "Socket Port", base.DEC)
f.sock_text = ProtoField.string("ka9qctl.sock.text", "Socket")

f.list_u32  = ProtoField.uint32("ka9qctl.list.u32", "List Element (u32)", base.DEC)
f.list_f32  = ProtoField.float("ka9qctl.list.f32", "List Element (f32)")

-- ---- TLV name table (from your header) ----
local STATUS_TYPE_NAME = {
  [0] = "EOL",
  [1] = "COMMAND_TAG",
  [2] = "CMD_CNT",
  [3] = "GPS_TIME",
  [4] = "DESCRIPTION",
  [5] = "STATUS_DEST_SOCKET",
  [6] = "SETOPTS",
  [7] = "CLEAROPTS",
  [8] = "RTP_TIMESNAP",
  [9] = "UNUSED4",
  [10] = "INPUT_SAMPRATE",
  [11] = "UNUSED6",
  [12] = "SPECTRUM_AVG",
  [13] = "INPUT_SAMPLES",
  [14] = "WINDOW_TYPE",
  [15] = "NOISE_BW",
  [16] = "OUTPUT_DATA_SOURCE_SOCKET",
  [17] = "OUTPUT_DATA_DEST_SOCKET",
  [18] = "OUTPUT_SSRC",
  [19] = "OUTPUT_TTL",
  [20] = "OUTPUT_SAMPRATE",
  [21] = "OUTPUT_METADATA_PACKETS",
  [22] = "OUTPUT_DATA_PACKETS",
  [23] = "OUTPUT_ERRORS",
  [24] = "CALIBRATE",
  [25] = "LNA_GAIN",
  [26] = "MIXER_GAIN",
  [27] = "IF_GAIN",
  [28] = "DC_I_OFFSET",
  [29] = "DC_Q_OFFSET",
  [30] = "IQ_IMBALANCE",
  [31] = "IQ_PHASE",
  [32] = "DIRECT_CONVERSION",
  [33] = "RADIO_FREQUENCY",
  [34] = "FIRST_LO_FREQUENCY",
  [35] = "SECOND_LO_FREQUENCY",
  [36] = "SHIFT_FREQUENCY",
  [37] = "DOPPLER_FREQUENCY",
  [38] = "DOPPLER_FREQUENCY_RATE",
  [39] = "LOW_EDGE",
  [40] = "HIGH_EDGE",
  [41] = "KAISER_BETA",
  [42] = "FILTER_BLOCKSIZE",
  [43] = "FILTER_FIR_LENGTH",
  [44] = "FILTER2",
  [45] = "IF_POWER",
  [46] = "BASEBAND_POWER",
  [47] = "NOISE_DENSITY",
  [48] = "DEMOD_TYPE",
  [49] = "OUTPUT_CHANNELS",
  [50] = "INDEPENDENT_SIDEBAND",
  [51] = "PLL_ENABLE",
  [52] = "PLL_LOCK",
  [53] = "PLL_SQUARE",
  [54] = "PLL_PHASE",
  [55] = "PLL_BW",
  [56] = "ENVELOPE",
  [57] = "SNR_SQUELCH",
  [58] = "PLL_SNR",
  [59] = "FREQ_OFFSET",
  [60] = "PEAK_DEVIATION",
  [61] = "PL_TONE",
  [62] = "AGC_ENABLE",
  [63] = "HEADROOM",
  [64] = "AGC_HANGTIME",
  [65] = "AGC_RECOVERY_RATE",
  [66] = "FM_SNR",
  [67] = "AGC_THRESHOLD",
  [68] = "GAIN",
  [69] = "OUTPUT_LEVEL",
  [70] = "OUTPUT_SAMPLES",
  [71] = "OPUS_BIT_RATE",
  [72] = "MINPACKET",
  [73] = "FILTER2_BLOCKSIZE",
  [74] = "FILTER2_FIR_LENGTH",
  [75] = "FILTER2_KAISER_BETA",
  [76] = "SPECTRUM_FFT_N",
  [77] = "FILTER_DROPS",
  [78] = "LOCK",
  [79] = "TP1",
  [80] = "TP2",
  [81] = "GAINSTEP",
  [82] = "AD_BITS_PER_SAMPLE",
  [83] = "SQUELCH_OPEN",
  [84] = "SQUELCH_CLOSE",
  [85] = "PRESET",
  [86] = "DEEMPH_TC",
  [87] = "DEEMPH_GAIN",
  [88] = "CONVERTER_OFFSET",
  [89] = "PL_DEVIATION",
  [90] = "THRESH_EXTEND",
  [91] = "SPECTRUM_SHAPE",
  [92] = "COHERENT_BIN_SPACING",
  [93] = "RESOLUTION_BW",
  [94] = "BIN_COUNT",
  [95] = "CROSSOVER",
  [96] = "BIN_DATA",
  [97] = "RF_ATTEN",
  [98] = "RF_GAIN",
  [99] = "RF_AGC",
  [100] = "FE_LOW_EDGE",
  [101] = "FE_HIGH_EDGE",
  [102] = "FE_ISREAL",
  [103] = "BLOCKS_SINCE_POLL",
  [104] = "AD_OVER",
  [105] = "RTP_PT",
  [106] = "STATUS_INTERVAL",
  [107] = "OUTPUT_ENCODING",
  [108] = "SAMPLES_SINCE_OVER",
  [109] = "PLL_WRAPS",
  [110] = "RF_LEVEL_CAL",
  [111] = "OPUS_DTX",
  [112] = "OPUS_APPLICATION",
  [113] = "OPUS_BANDWIDTH",
  [114] = "OPUS_FEC",
}

-- Reverse lookup: name -> type ID
local name_to_id = {}
for k, v in pairs(STATUS_TYPE_NAME) do
  name_to_id[v] = k
end


-- ---- TLV kind table (inferred from your dump.c) ----
-- Kinds: "uint", "float64" (len 4/8 chosen at runtime), "string", "socket", "f32_list", "gps_ns"
local TLV_KIND = {
  [1] = "uint",
  [2] = "uint",
  [3] = "gps_ns",
  [4] = "string",
  [5] = "socket",
  [6] = "uint",
  [7] = "uint",
  [8] = "uint",
  [10] = "uint",
  [12] = "uint",
  [13] = "uint",
  [14] = "uint",
  [15] = "float32",
  [16] = "socket",
  [17] = "socket",
  [18] = "uint",
  [19] = "uint",
  [20] = "uint",
  [21] = "uint",
  [22] = "uint",
  [23] = "uint",
  [24] = "float64",
  [25] = "uint",
  [26] = "uint",
  [27] = "uint",
  [28] = "float32",
  [29] = "float32",
  [30] = "float32",
  [31] = "float32",
  [32] = "bool",
  [33] = "float64",
  [34] = "float64",
  [35] = "float64",
  [36] = "float32",
  [37] = "float32",
  [38] = "float32",
  [39] = "float32",
  [40] = "float32",
  [41] = "float32",
  [42] = "uint",
  [43] = "uint",
  [44] = "uint",
  [45] = "float32",
  [46] = "float32",
  [47] = "float32",
  [48] = "uint",
  [49] = "uint",
  [50] = "bool",
  [51] = "bool",
  [52] = "bool",
  [53] = "bool",
  [54] = "float32",
  [55] = "float32",
  [56] = "bool",
  [57] = "bool",
  [58] = "float32",
  [59] = "float32",
  [60] = "float32",
  [61] = "float32",
  [62] = "bool",
  [63] = "float32",
  [64] = "float32",
  [65] = "float32",
  [66] = "float32",
  [67] = "float32",
  [68] = "float32",
  [69] = "float32",
  [70] = "uint",
  [71] = "uint",
  [72] = "uint",
  [75] = "float32",
  [77] = "uint",
  [78] = "uint",
  [79] = "float32",
  [80] = "float32",
  [81] = "uint",
  [82] = "uint",
  [83] = "float32",
  [84] = "float32",
  [85] = "string",
  [86] = "float32",
  [87] = "float32",
  [88] = "float32",
  [89] = "float32",
  [90] = "bool",
  [91] = "float32",
  [92] = "float32",
  [93] = "float32",
  [94] = "uint",
  [95] = "float32",
  [96] = "f32_list",
  [97] = "float32",
  [98] = "float32",
  [99] = "bool",
  [100] = "float32",
  [101] = "float32",
  [102] = "bool",
  [103] = "uint",
  [104] = "uint",
  [105] = "uint",
  [106] = "uint",
  [107] = "uint",
  [108] = "uint",
  [109] = "uint",
  [110] = "float32",
  [111] = "bool",
  [112] = "uint",
  [113] = "uint",
  [114] = "uint",
}

-- ---- Helpers ----

local function parse_ber_length(tvb, offset, pktlen)
  if offset >= pktlen then return nil, 0, "Missing length byte" end
  local first = tvb(offset,1):uint()
  if first < 0x80 then
    return first, 1, nil
  end
  local n = bit.band(first, 0x7f)
  if n == 0 then
    return nil, 1, "Indefinite length not supported"
  end
  if n > 4 then
    return nil, 1 + n, string.format("Length-of-length too large (%u)", n)
  end
  if offset + 1 + n > pktlen then
    return nil, 1 + n, "Truncated length field"
  end
  local len = 0
  for i = 0, n-1 do
    len = len * 256 + tvb(offset + 1 + i, 1):uint()
  end
  return len, 1 + n, nil
end

local function decode_uint_anylen_be(v)
  local n = v:len()
  if n == 0 then return UInt64(0), nil end
  if n > 8 then
    return nil, string.format("uint too long (%u bytes)", n)
  end
  return v:uint64(), nil  -- big-endian, 1..8 bytes
end

local function add_float32(tree, tvb_range, little)
  if tvb_range:len() ~= 4 then
    tree:add(f.tlv_raw, tvb_range):append_text(" (expected 4 bytes for float32)")
    return
  end
  if little then tree:add_le(f.float, tvb_range) else tree:add(f.float, tvb_range) end
end

local function add_float64(tree, tvb_range, little)
  if tvb_range:len() ~= 8 then
    tree:add(f.tlv_raw, tvb_range):append_text(" (expected 8 bytes for float64)")
    return
  end
  if little then tree:add_le(f.double, tvb_range) else tree:add(f.double, tvb_range) end
end

local function unpack_f32(v, little)
  local L = v:len()
  if L == 0 then
    return 0.0, nil
  end
  if L > 4 then
    return nil,"float32 len>4"
  end
  local raw = v:bytes():raw()        -- exact 4 bytes
  raw = string.rep("\0", 4 - L) .. raw
  local x = string.unpack(">f", raw)
  return x, nil
end

local function unpack_f64(v, little)
  local L = v:len()
  if L == 0 then
    return 0.0, nil
  end
  if L > 8 then
    return nil,"float64 len>8"
  end
  local raw = v:bytes():raw()        -- exact 4 bytes
  raw = string.rep("\0", 8 - L) .. raw
  local x = string.unpack(">d", raw)
  return x, nil
end


local function decode_socket_text(v)
  local L = v:len()
  if L == 6 then
    local a = v(0,4)
    local p = v(4,2):uint()
    return string.format("%u.%u.%u.%u:%u",
      a(0,1):uint(), a(1,1):uint(), a(2,1):uint(), a(3,1):uint(), p)
  end
  if L == 10 then
    local a = v(0,8)
    local p = v(8,2):uint()
    local s = {
      string.format("%02x%02x", a(0,1):uint(), a(1,1):uint()),
      string.format("%02x%02x", a(2,1):uint(), a(3,1):uint()),
      string.format("%02x%02x", a(4,1):uint(), a(5,1):uint()),
      string.format("%02x%02x", a(6,1):uint(), a(7,1):uint()),
    }
    return table.concat(s, ":") .. ":" .. tostring(p)
  end
  if L == 18 then
    local p = v(16,2):uint()
    return string.format("[IPv6]:%u", p)
  end
  return string.format("socket(len=%u)", L)
end

local function add_socket(tree, tvb_range)
  local L = tvb_range:len()
  if L == 6 then
    -- IPv4 (4) + port (2)
    local a = tvb_range(0,4)
    local p = tvb_range(4,2):uint()
    local txt = string.format("%u.%u.%u.%u:%u",
      a(0,1):uint(), a(1,1):uint(), a(2,1):uint(), a(3,1):uint(), p)
    tree:add(f.sock_text, tvb_range, txt)
    tree:add(f.sock_addr4, tvb_range(0,4))
    tree:add(f.sock_port, tvb_range(4,2))
    return
  end

  if L == 10 then
    -- "compact IPv6": 8 bytes + port (2) as you described.
    -- Wireshark cannot represent this as a true IPv6 address field (needs 16 bytes), so show text.
    local a = tvb_range(0,8)
    local p = tvb_range(8,2):uint()
    local s = {
      string.format("%02x%02x", a(0,1):uint(), a(1,1):uint()),
      string.format("%02x%02x", a(2,1):uint(), a(3,1):uint()),
      string.format("%02x%02x", a(4,1):uint(), a(5,1):uint()),
      string.format("%02x%02x", a(6,1):uint(), a(7,1):uint()),
    }
    local txt = table.concat(s, ":") .. ":" .. tostring(p)
    tree:add(f.sock_text, tvb_range, txt)
    tree:add(f.sock_port, tvb_range(8,2))
    return
  end

  if L == 18 then
    -- Full IPv6 (16) + port (2)
    tree:add(f.sock_addr6, tvb_range(0,16))
    tree:add(f.sock_port, tvb_range(16,2))
    return
  end

  tree:add(f.tlv_raw, tvb_range):append_text(" (expected 6=v4, 10=compact-v6, or 18=v6)")
end

local GPS_UNIX_EPOCH_OFFSET = 315964800  -- seconds between 1970-01-01 and 1980-01-06
local GPS_UTC_OFFSET_SECONDS = 18        -- current GPS-UTC offset; informational only (not tracked dynamically)

local function format_gps_ns(u64)
  -- nanoseconds since GPS epoch; keep it simple and safe
  local ns = u64:tonumber()
  if not ns then
    return "gps_ns=" .. tostring(u64)
  end
  -- If it won't fit exactly in a Lua double, don't pretend.
  if ns > 9.0e15 then
    return "gps_ns=" .. tostring(u64)
  end
  local gps_sec = math.floor(ns / 1e9)
  local rem_ns = ns - gps_sec * 1e9
  local unix_utc = gps_sec + GPS_UNIX_EPOCH_OFFSET - GPS_UTC_OFFSET_SECONDS
  return string.format("gps=%d.%09d (≈UTC unix %d)", gps_sec, rem_ns, unix_utc)
end

local function bool_text(u)
  return (u ~= 0) and "true" or "false"
end

local function add_f32_list(tree, tvb_range, little)
  local n = tvb_range:len()
  if (n % 4) ~= 0 then
    tree:add(f.tlv_raw, tvb_range):append_text(" (BIN_DATA len not multiple of 4)")
    return
  end
  local count = n / 4
  local lt = tree:add(tvb_range, string.format("BIN_DATA (%u float32)", count))
  for i = 0, count - 1 do
    local r = tvb_range(i*4, 4)
    if little then lt:add_le(f.list_f32, r) else lt:add(f.list_f32, r) end
  end
end

local function decode_by_kind(st, t, v, prefs)
  local kind = TLV_KIND[t] or "raw"
  if kind == "bool" then
    local u, err = decode_uint_anylen_be(v)    
    if not u then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    local b = (u ~= 0)
    st:add(f.uint, v, u)
    return b and "true" or "false"
  elseif kind == "uint" then
    local u, err = decode_uint_anylen_be(v)
    if not u then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, u)
    return tostring(u)
  elseif kind == "gps_ns" then
    local u, err = decode_uint_anylen_be(v)
    if not u then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, u):append_text(" (GPS ns)")
    return format_gps_ns(u)
  elseif kind == "float32" then
    local x, err = unpack_f32(v, prefs.float_little_endian)
    if x then
      st:add(f.float,v,x)
      st:append_text(string.format(" = %.9g", x))
    else
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
    end
  elseif kind == "float64" then
    local x, err = unpack_f64(v, prefs.float_little_endian)
    if x then
      st:add(f.double, v, x)
      st:append_text(string.format(" = %.17g", x))
    else
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
    end
  elseif kind == "string" then
    local s = v:string()
    st:add(f.str, v, s)
    local ss = s
    if #ss > 60 then ss = ss:sub(1,60) .. "…" end
    return '"' .. ss .. '"'
  elseif kind == "socket" then
    local txt = decode_socket_text(v)
    add_socket(st, v)
    return txt
  elseif kind == "f32_list" then
    for i = 0, v:len()-4, 4 do
      local r = v(i,4)
      local x = unpack_f32(r, prefs.float_little_endian)
      if x then
	lt:add(f.list_f32, r, x)
      else
	lt:add(f.tlv_raw, r)
      end
    end
  else
    st:add(f.tlv_raw, v)
    return nil
  end
end


-- ---- Main dissector ----
function ka9q.dissector(tvb, pinfo, tree)
  local pktlen = tvb:len()
  if pktlen < 1 then return end

  pinfo.cols.protocol = "KA9QCTL"

  local prefs = {
    float_little_endian = ka9q.prefs.float_little_endian,
  }

  local root = tree:add(ka9q, tvb(), "ka9q-radio Control/Status")

  if ka9q.prefs.show_raw_packet then
    root:add(f.raw_packet, tvb())
  end

  local offset = 0
  local msg_class = tvb(offset,1):uint()
  root:add(f.msg_class, tvb(offset,1))
  offset = offset + 1

  local info_parts = {}
  if ka9q.prefs.add_info_column then
    table.insert(info_parts, (msg_class == 1) and "CMD" or "STAT")
  end

  while offset < pktlen do
    local opt = tvb(offset,1):uint()
    if opt == 0 then
      break
    elseif offset + 2 > pktlen then
      root:add_expert_info(PI_MALFORMED, PI_ERROR, "Truncated TLV header at end of packet")
      break
    end

    local t = tvb(offset,1):uint()
    offset = offset + 1

    local len, len_consumed, err = parse_ber_length(tvb, offset, pktlen)
    if err then
      root:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      break
    end
    offset = offset + len_consumed

    if offset + len > pktlen then
      root:add_expert_info(PI_MALFORMED, PI_ERROR,
        string.format("TLV type %u length %u overruns packet", t, len))
      break
    end

    local v = tvb(offset, len)
    offset = offset + len

    local tlv_name = STATUS_TYPE_NAME[t] or "Unknown"
    local st = root:add(v, string.format("(%u) %s", t, tlv_name, len))
    st:add(f.tlv_type, tvb(offset - len - len_consumed - 1, 1))
    st:add(f.tlv_len,  tvb(offset - len - len_consumed, len_consumed), len)

    local summary = decode_by_kind(st, t, v, prefs)
    if summary and summary ~= "" then
      st:append_text(" = " .. summary)
    end

    -- Info column: show COMMAND_TAG and GPS_TIME
    if ka9q.prefs.add_info_column then
      if t == (name_to_id["COMMAND_TAG"] or -1) then
        local u, _ = decode_uint_anylen_be(v)
        if u then table.insert(info_parts, string.format("tag=0x%s", u:tohex())) end
      elseif t == (name_to_id["GPS_TIME"] or -1) and summary then
        table.insert(info_parts, summary)
      end
    end

    -- EOL terminator: stop immediately (warn on trailing bytes)
    if t == 0 then
      if offset < pktlen then
        root:add_expert_info(PI_PROTOCOL, PI_WARN,
          string.format("Extra %u byte(s) after EOL", pktlen - offset))
      end
      break
    end
  end

  if ka9q.prefs.add_info_column then
    pinfo.cols.info = table.concat(info_parts, " ")
  end
end

-- Register to UDP port 5006
DissectorTable.get("udp.port"):add(5006, ka9q)
