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

f.list_uint  = ProtoField.uint32("ka9qctl.list.uint", "List Element (uint)", base.DEC)
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
  [1] = "uint_hex",
  [2] = "uint",
  [3] = "gps_ns",
  [4] = "string",
  [5] = "socket",
  [6] = "uint",
  [7] = "uint",
  [8] = "uint",
  [10] = "uint_hz",
  [12] = "uint",
  [13] = "uint",
  [14] = "window",
  [15] = "f32_hz",
  [16] = "socket",
  [17] = "socket",
  [18] = "uint",
  [19] = "uint",
  [20] = "uint_hz",
  [21] = "uint",
  [22] = "uint",
  [23] = "uint",
  [24] = "f64_hz",
  [25] = "uint_db",
  [26] = "uint_db",
  [27] = "uint_db",
  [28] = "float32",
  [29] = "float32",
  [30] = "float32",
  [31] = "float32",
  [32] = "bool",
  [33] = "f64_hz",
  [34] = "f64_hz",
  [35] = "f64_hz",
  [36] = "f64_hz",
  [37] = "f64_hz",
  [38] = "f64_hz_per_s",
  [39] = "f32_hz",
  [40] = "f32_hz",
  [41] = "float32",
  [42] = "uint",
  [43] = "uint",
  [44] = "uint",
  [45] = "f32_dbfs",
  [46] = "f32_db",
  [47] = "f32_dbmj",
  [48] = "demod",
  [49] = "uint",
  [50] = "bool",
  [51] = "bool",
  [52] = "bool",
  [53] = "bool",
  [54] = "float32",
  [55] = "f32_hz",
  [56] = "bool",
  [57] = "bool",
  [58] = "f32_db",
  [59] = "f32_hz",
  [60] = "f32_hz",
  [61] = "f32_hz",
  [62] = "bool",
  [63] = "f32_db",
  [64] = "f32_s",
  [65] = "f32_db_per_s",
  [66] = "f32_db",
  [67] = "f32_db",
  [68] = "f32_db",
  [69] = "f32_dbfs",
  [70] = "uint",
  [71] = "uint_bps",
  [72] = "uint",
  [73] = "uint",
  [74] = "uint",
  [75] = "f32",
  [76] = "uint",
  [77] = "uint",
  [78] = "bool",
  [79] = "float32",
  [80] = "float32",
  [81] = "uint",
  [82] = "uint",
  [83] = "f32_db",
  [84] = "f32_db",
  [85] = "string",
  [86] = "f32_s",
  [87] = "f32_db",
  [88] = "f32_hz",
  [89] = "f32_hz",
  [90] = "bool",
  [91] = "float32",
  [92] = "f32_hz",
  [93] = "f32_hz",
  [94] = "uint",
  [95] = "f32_hz",
  [96] = "f32_list",
  [97] = "f32_db",
  [98] = "f32_db",
  [99] = "bool",
  [100] = "f32_hz",
  [101] = "f32_hz",
  [102] = "bool",
  [103] = "uint",
  [104] = "uint",
  [105] = "uint",
  [106] = "uint",
  [107] = "encoding",
  [108] = "uint",
  [109] = "uint",
  [110] = "f32_dbm",
  [111] = "bool",
  [112] = "opus_app",
  [113] = "opus_bw",
  [114] = "uint",
}

-- ---- Helpers ----

local DEMOD = {
  [0] = "Linear",
  "FM",
  "WFM",
  "Spectrum"
}

local ENCODING = {
  [0] = "None",
  "S16LE",
  "S16BE",
  "OPUS",
  "F32LE",
  "AX25",
  "F16LE",
  "OPUS_VOIP",
}


local OPUS_BW = {
  [1101] = "Narrowband (4 kHz)",   -- example numbers, replace with yours
  [1102] = "Mediumband (6 kHz)",
  [1103] = "Wideband (8 kHz)",
  [1104] = "Superwideband (12 kHz)",
  [1105] = "Fullband (20 kHz)",
}
local OPUS_APPLICATION = {
  [2048] = "VOIP",
  [2049] = "AUDIO",
  [2051] = "RESTRICTED_LOWDELAY",
  [2052] = "RESTRICTED_SILK",
  [2053] = "RESTRICTED_CELT",
}

local WINDOWS = {
  [0] = "Kaiser",
  "Rectangular",
  "Blackman",
  "exact Blackman",
  "Gaussian",
  "Hann",
  "Hamming",
}

local function fmt_opus_bw(u)
  return OPUS_BW[u] or ("unknown (" .. tostring(u) .. ")")
end

-- If OPUS_BANDWIDTH is encoded as uint:
-- FORMAT[name_to_id["OPUS_BANDWIDTH"]] = fmt_opus_bw


local function group_dec(s)
  -- s = "123456789"
  local sign = ""
  if s:sub(1,1) == "-" then
    sign = "-"
    s = s:sub(2)
  end

  local out = s:reverse():gsub("(%d%d%d)", "%1 "):reverse()
  out = out:gsub("^ ", "")  -- remove leading space if any
  return sign .. out
end

local function group_hex(s)
  -- s = "deadbeef"
  local out = s:reverse():gsub("(%x%x%x%x)", "%1 "):reverse()
  out = out:gsub("^ ", "")
  return out
end

local function group_float(s)
  local sign = ""
  if s:sub(1,1) == "-" then
    sign = "-"
    s = s:sub(2)
  end

  local int, frac = s:match("^(%d+)%.(%d+)$")
  if frac then
    return sign .. group_dec(int) .. "." .. frac
  end

  -- No decimal point
  return sign .. group_dec(s)
end

local function fmt_bitrate(x)
  if x == 0 then return "0 bps" end
  if math.abs(x) >= 1000 then
    return string.format("%.1f kbps", x/1000.0)
  end
  return string.format("%.0f bps", x)
end

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

local function decode_uint(v)
  local n = v:len()
  if n == 0 then return UInt64(0), nil end
  if n > 8 then
    return nil, string.format("uint too long (%u bytes)", n)
  end
  return v:uint64(), nil  -- big-endian, 1..8 bytes
end

local function decode_f32(v)
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

local function decode_f64(v)
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

local function fmt_utc_seconds(x)
  -- x: seconds since Unix epoch, may be integer or float
  local sec = math.floor(x)
  local frac = x - sec

  local t = os.date("!%Y-%m-%d %H:%M:%S", sec)
  if not t then
    return "invalid time"
  end

  if frac > 0 then
    return string.format("%s.%03d UTC", t, math.floor(frac * 1000 + 0.5))
  end

  return t
end



local GPS_UNIX_EPOCH_OFFSET = 315964800  -- seconds between 1970-01-01 and 1980-01-06
local GPS_UTC_OFFSET_SECONDS = 18        -- current GPS-UTC offset; informational only (not tracked dynamically)

local function format_gps_ns(u64)
  -- nanoseconds since GPS epoch; keep it simple and safe
  local ns = u64:tonumber()
  if not ns then
    return tostring(u64)
  end
  -- If it won't fit exactly in a Lua double, don't pretend.
  local gps_sec = math.floor(ns / 1e9)
  local rem_ns = ns - gps_sec * 1e9
  local unix_utc = gps_sec + GPS_UNIX_EPOCH_OFFSET - GPS_UTC_OFFSET_SECONDS
  local ns_string = group_dec(string.format("%09d",rem_ns))
  return fmt_utc_seconds(ns/1e9 - GPS_UTC_OFFSET_SECONDS) .. "; " .. group_dec(tostring(gps_sec)) .. "." .. ns_string .. " GPS seconds"
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

local function decode_by_kind(kind, v, st, t)
  if kind == "demod" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    local d = DEMOD[x:tonumber()]
    if not d then
      return "invalid"
    else
      return d
    end

  elseif kind == "window" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    local e = WINDOWS[x:tonumber()]
    if not e then
      return "invalid"
    else
      return e
    end

  elseif kind == "encoding" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    local e = ENCODING[x:tonumber()]
    if not e then
      return "invalid"
    else
      return e
    end

  elseif kind == "opus_bw" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    local bw = OPUS_BW[x:tonumber()]
    if not bw then
      return "invalid"
    else
      return bw
    end

  elseif kind == "opus_app" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    local bw = OPUS_APPLICATION[x:tonumber()]
    if not bw then
      return "invalid"
    else
      return bw
    end

  elseif kind == "uint_hz" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    return group_dec(tostring(x)) .. " Hz"

  elseif kind == "uint_bps" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    return group_dec(tostring(x)) .. " bps"

  elseif kind == "f32_hz" then
    local x, err = decode_f32(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.float, v, x)
    return group_float(string.format("%.1f",x)) .. " Hz"

  elseif kind == "f64_hz" then
    local x, err = decode_f64(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.double, v, x)
    return group_float(string.format("%.3f",x)) .. " Hz"

  elseif kind == "f64_hz_per_s" then
    local x, err = decode_f64(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.double, v, x)
    return group_float(string.format("%.6f", x)) .. " Hz/s"

  elseif kind == "uint_db" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    return group_dec(tostring(x)) .. " dB"

  elseif kind == "f32_db" then
    local x, err = decode_f32(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.float, v, x)
    return group_float(string.format("%.1f", x)) .. " dB"

  elseif kind == "f32_db_per_s" then
    local x, err = decode_f32(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.float, v, x)
    return group_float(string.format("%.1f", x)) .. " dB/s"

  elseif kind == "f32_dbm" then
    local x, err = decode_f32(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.float, v, x)
    return group_float(string.format("%.1f", x)) .. " dBm"

  elseif kind == "f32_dbmj" then
    local x, err = decode_f32(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.float, v, x)
    return group_float(string.format("%.1f",x)) .. " dBmJ (dBm/Hz)"

  elseif kind == "f32_dbfs" then
    local x, err = decode_f32(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.float, v, x)
    return group_float(string.format("%.1f",x)) .. " dBFS"

  elseif kind == "f32_s" then
    local x, err = decode_f32(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.float, v, x)
    return group_float(string.format("%.1f", x)) .. " s"

  elseif kind == "bool" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    if(x == UInt64(0)) then
      return "false"
    else
      return "true"
    end

  elseif kind == "uint" then
    -- dimensionless integer
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    return group_dec(tostring(x))

  elseif kind == "uint_hex" then
    -- dimensionless hex integer
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v, x)
    local h = x:tohex():gsub("^0+", "")
    return group_hex((h ~= "" and h or "0"))

  elseif kind == "gps_ns" then
    local x, err = decode_uint(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.uint, v,x):append_text(" (GPS ns)")
    return format_gps_ns(x)

  elseif kind == "float32" then
    -- dimensionless
    local x, err = decode_f32(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.float,v,x)
    return group_float(tostring(x))

  elseif kind == "float64" then
    -- dimensionless
    local x, err = decode_f64(v)
    if not x then
      st:add_expert_info(PI_MALFORMED, PI_ERROR, err)
      st:add(f.tlv_raw, v)
      return nil
    end
    st:add(f.double, v, x)
    return group_float(tostring(x))

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
    idx = 0
    for i = 0, v:len()-4, 4 do
      local r = v(i,4)
      local x = decode_f32(r)
      if x then
	local ei = st:add(f.list_f32, r, x)
	ei:append_text(string.format(" [%d]",idx))
      else
	st:add(f.tlv_raw, r)
      end
      idx = idx+1
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
    local kind = TLV_KIND[t]
    local st = root:add(v, string.format("(%u) %s", t, tlv_name, v, len))
    st:add(f.tlv_type, tvb(offset - len - len_consumed - 1, 1))
    st:add(f.tlv_len,  tvb(offset - len - len_consumed, len_consumed), len)

    local summary = decode_by_kind(kind, v, st, t)
    if summary and summary ~= "" then
      st:append_text(" = " .. summary)
    end
    -- Info column: show COMMAND_TAG and GPS_TIME
    if ka9q.prefs.add_info_column then
      if t == (name_to_id["OUTPUT_SSRC"] or -1) then
	local u, _ = decode_uint(v)
	if u then
	  local tag = "ssrc=" .. tostring(u)
	  table.insert(info_parts, tag)
	end
      elseif t == (name_to_id["COMMAND_TAG"] or -1) then
        local u, _ = decode_uint(v)
	if u then
	  local h = u:tohex():gsub("^0+", "")
	  local tag = "tag=" .. group_hex((h ~= "" and h or "0"))
	  table.insert(info_parts, tag)
	end
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
