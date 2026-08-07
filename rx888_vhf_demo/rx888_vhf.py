#!/usr/bin/env python3
"""
rx888_vhf.py — Shared RX888 mk2 VHF front-end driver module.

Portable driver core: EP0 transport + Si5351 CLKB + R828D tuner control.
Extracted from vhf_tune.py for reuse by both the CLI (vhf_tune.py) and the
interactive TUI (vhf_fm_radio.py).

See vhf_tune.py docstring for porting notes, bit-order gotchas, and the
bench-learned lessons that shaped this driver.

R820T2 / R828D references (Rafael Micro never published a full datasheet):
  - Closest register descriptions:
      https://www.rtl-sdr.com/wp-content/uploads/2016/12/R820T2_Register_Description.pdf
  - Closest datasheet:
      https://datasheet4u.com/download/1469947/R820T2.html
  - Reference driver code:
      osmocom rtl-sdr  https://github.com/osmocom/rtl-sdr/blob/master/src/tuner_r82xx.c
      Linux kernel     https://github.com/torvalds/linux/blob/master/drivers/media/tuners/r820t.c
  These describe the R820T2 — the closest available docs/code for the R828D. In
  practice that's sufficient: the drivers appear to operate the R828D just as
  well as the R820T (same register map and tuning core; chip-specific
  differences are a few registers, e.g. Air-In/Cable1 select at 0x05[6:5]).

Requires: pyusb  (pip install pyusb)
"""

import struct, sys, time, subprocess
try:
    import usb.core
except ImportError:
    usb = None

# ── Device identity ────────────────────────────────────────────────────
RX888_VID      = 0x04B4
RX888_PID      = 0x00F1          # application firmware
RX888_PID_BOOT = 0x00F3          # Cypress bootloader

# ── EP0 vendor request codes (protocol.h) ──────────────────────────────
TESTFX3  = 0xAC                  # IN  -> [hwconfig, fw_hi, fw_lo, vendor_rqt_count]
GPIOFX3  = 0xAD                  # OUT <- 4-byte LE GPIO control word
I2CWFX3  = 0xAE                  # OUT <- I2C write: wValue=dev, wIndex=reg, data
I2CRFX3  = 0xAF                  # IN  -> I2C read:  wValue=dev, wIndex=reg, wLength=n
STARTADC = 0xB2                  # OUT <- 4-byte LE ADC sample rate
GETSTATS = 0xB3                  # IN  -> diagnostics; gpio_state at [26:30] (payload >= 30B)
BM_OUT   = 0x40                  # bmRequestType: host->device | vendor | device
BM_IN    = 0xC0                  # bmRequestType: device->host | vendor | device

# ── GPIO control-word bits (protocol.h enum GPIOPin) ───────────────────
BIAS_HF  = 1 << 8
BIAS_VHF = 1 << 9
VHF_EN   = 1 << 15               # HF/VHF antenna switch (set = VHF)

# ── I2C device addresses (8-bit, as the firmware's I2cTransfer uses) ───
R828D_ADDR  = 0x74
SI5351_ADDR = 0xC0

# ── Board constants (RX888R2Radio.cpp / Si5351.c) ──────────────────────
R828D_REF_HZ = 16_000_000        # R828D reference fed via Si5351 CLKB
IF_CARRIER   = 4_570_000         # IF center with the 8 MHz channel filter
SI5351_XTAL  = 27_000_000        # Si5351 crystal

# ── Si5351 register addresses ──────────────────────────────────────────
SI_PLL_B     = 34
SI_MS2       = 58                # multisynth for CLK2 (= CLKB)
SI_PLL_RESET = 177
SI_CLK2      = 18                # CLK2 control (bit7 = power-down)

# ── R828D init register block, regs 0x05..0x1f ─────────────────────────
# [tune]/[bw] = a runtime function (set_freq/set_pll/set_mux/set_bandwidth)
# rewrites this register before it's used, so the seed only fixes the bits
# those masked writes leave untouched; the rest are set-once config.
# AGC polarity (bench-confirmed on RX888 mk2): 0x05[4] and 0x07[4] are both
# 1=auto / 0=manual, so init leaves LNA AGC off (0x05 bit4=0) and mixer AGC
# on (0x07 bit4=1).
R828D_INIT_BASE = 0x05
# Old opaque form, kept for reference:
# R828D_INIT = [
#     0x80,0x13,0x70,0xC0,0x40,0xDB,0x6B,0xEB,0x53,0x75,0x68,0x6C,0xBB,
#     0x80,0x31,0x0F,0x00,0xC0,0x30,0x48,0xEC,0x60,0x00,0x24,0xDD,0x0E,0x40,
# ]
R828D_INIT = [
    0x80,  # 0x05 LNA   [tune] gain/mode + Air/Cable; loop-through off, manual, AGC off
    0x13,  # 0x06       power-det1 on, det3 off, filter-gain 0 dB, LNA power 3
    0x70,  # 0x07 Mixer [tune] gain/sideband; mixer on, AGC auto (on)
    0xC0,  # 0x08       mixer buffer on, low current, image-gain trim 0
    0x40,  # 0x09       IF filter on, low current, image-phase trim 0
    0xDB,  # 0x0A       [bw] channel-filter fine BW   (set_bandwidth + cal)
    0x6B,  # 0x0B       [bw] channel-filter coarse BW (set_bandwidth)
    0xEB,  # 0x0C       IF VGA on, gain by code, VGA_CODE=0x0B (~26.5 dB)
    0x53,  # 0x0D       LNA AGC detector thresholds (hi 5, lo 3)
    0x75,  # 0x0E       mixer AGC detector thresholds (hi 7, lo 5)
    #0x68,  # 0x0F       librtlsdr: bit6=1, CLK_OUT_ENB=0
    0x38,  # 0x0F       ka9q: bit6=0, CLK_OUT_ENB=1
    0x6C,  # 0x10       [tune] PLL divider/refdiv/xtal-cap (set_mux+set_pll)
    #0xBB,  # 0x11       librtlsdr: CP_CUR=7 (auto)
    0xAB,  # 0x11       ka9q: CP_CUR=5 (fixed)
    0x80,  # 0x12       [tune] VCO current/dither (set_pll)
    0x31,  # 0x13       VCO auto mode; low 6 bits = version tag (ignored in auto)
    0x0F,  # 0x14       [tune] PLL nint (set_pll)
    0x00,  # 0x15       [tune] PLL sdm lo (set_pll)
    0xC0,  # 0x16       [tune] PLL sdm hi (set_pll)
    0x30,  # 0x17       [partial] open-drain set by set_mux; rest = PLL dig LDO 1.8V/8mA
    0x48,  # 0x18       ring oscillator OFF
    0xEC,  # 0x19       RF tracking filter ON, poly-filter current
    0x60,  # 0x1A       [tune] RF mux/autotune (set_mux+set_pll)
    0x00,  # 0x1B       [tune] tracking-filter band tf_c (set_mux)
    0x24,  # 0x1C       mixer power-detector TOP
    #0xDD,  # 0x1D       librtlsdr: PDET1_GAIN=3
    0xED,  # 0x1D       ka9q: PDET1_GAIN=5 (more aggressive LNA AGC)
    0x0E,  # 0x1E       [bw] filter-extension bit (set_bandwidth)
    0x40,  # 0x1F       loop-through-att / ring-osc power (both unused — idle)
]

# ── R828D tracking-filter bands: (LO_start_MHz, open_d, rf_mux_ploy, tf_c)
FREQ_RANGES = [
    (  0,0x08,0x02,0xDF),( 50,0x08,0x02,0xBE),( 55,0x08,0x02,0x8B),( 60,0x08,0x02,0x7B),
    ( 65,0x08,0x02,0x69),( 70,0x08,0x02,0x58),( 75,0x00,0x02,0x44),( 80,0x00,0x02,0x44),
    ( 90,0x00,0x02,0x34),(100,0x00,0x02,0x34),(110,0x00,0x02,0x24),(120,0x00,0x02,0x24),
    (140,0x00,0x02,0x14),(180,0x00,0x02,0x13),(220,0x00,0x02,0x13),(250,0x00,0x02,0x11),
    (280,0x00,0x02,0x00),(310,0x00,0x41,0x00),(450,0x00,0x41,0x00),(588,0x00,0x40,0x00),
    (650,0x00,0x40,0x00),
]

# ── Bandwidth presets: (reg_0x0A_val, reg_0x0B_val, reg_0x1E_val, if_center_hz)
#    Wide presets from hardcoded top of set_bandwidth; narrow from IFi[] table
#    (tuner_r82xx_explained.md §5). Keys are MHz (float for sub-MHz entries).
_BW_PRESETS = {
    8:    (0x10, 0x0B, 0x60, 4_570_000),
    7:    (0x10, 0x2A, 0x60, 4_570_000),
    6:    (0x10, 0x6B, 0x00, 3_570_000),
    5:    (0x0B, 0x6B, 0x00, 3_570_000),
    3:    (0x04, 0x8F, 0x00, 2_000_000),
    1.6:  (0x0F, 0x8B, 0x00, 1_900_000),
    0.6:  (0x0F, 0xEA, 0x00, 1_706_000),
    0.29: (0x0F, 0xE7, 0x00, 1_925_000),
}
BW_CYCLE = [8, 7, 6, 5, 3, 1.6, 0.6, 0.29]


class TuneError(Exception):
    """Bring-up failure (unreachable tuner, ID mismatch, no PLL lock, ...)."""


def _bitrev8(b):
    """Reverse the 8 bits of a byte. The R828D streams reads LSB-first, so a
    standard MSB-first I2C master (the FX3) receives each read byte reversed
    from the datasheet's logical bit order; this undoes that. Writes are NOT
    reversed (the chip takes writes MSB-first). Mirrors librtlsdr r82xx_read."""
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
    return b


# ════════════════════════════════════════════════════════════════════════
#  Portable driver core — this is what a C host driver re-implements.
# ════════════════════════════════════════════════════════════════════════
class RX888:
    def __init__(self):
        self.dev = usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID)
        if self.dev is None:
            sys.exit("RX888 mk2 not found (04B4:00F1).")
        self.regs = {}            # R828D register shadow (for masked writes)
        self.ref_hz = R828D_REF_HZ   # active R828D reference (CLKB); set by clkb_on
        self.if_hz = IF_CARRIER      # active IF center; set by set_bandwidth

    # ── EP0 transport (libusb control transfers) ──────────────────────────
    def _out_u32(self, cmd, value):              # GPIOFX3 / STARTADC payload
        self.dev.ctrl_transfer(BM_OUT, cmd, 0, 0, struct.pack("<I", value))

    def gpio(self, word):     self._out_u32(GPIOFX3, word)
    def start_adc(self, hz):  self._out_u32(STARTADC, hz)

    def i2c_w(self, addr, reg, data):
        self.dev.ctrl_transfer(BM_OUT, I2CWFX3, addr, reg, bytes(data))

    def i2c_r(self, addr, reg, n):
        return bytes(self.dev.ctrl_transfer(BM_IN, I2CRFX3, addr, reg, n))

    # ── diagnostics / verification ────────────────────────────────────────
    def check_alive(self):
        """Confirm the firmware answers vendor commands (beyond lsusb)."""
        try:
            hw, fwhi, fwlo, rqt = bytes(self.dev.ctrl_transfer(BM_IN, TESTFX3, 0, 0, 4))
        except usb.core.USBError as e:
            sys.exit(f"RX888 on USB but firmware not answering TESTFX3 ({e}). "
                     f"Powered up? claimed elsewhere?")
        print(f"alive: hwconfig=0x{hw:02X} fw={fwhi}.{fwlo} vendor_rqts={rqt}")
        if hw != 0x04:
            print(f"  WARNING: hwconfig 0x{hw:02X} is not RX888r2 (0x04)")
        return hw

    def read_gpio_state(self):
        """Live steady-state GPIO word from GETSTATS bytes [26:30] (packed with
        the GPIOFX3 bit positions). Returns None if the payload predates that
        field (< 30 bytes, e.g. release v0.1.0) so the caller requires --base."""
        buf = bytes(self.dev.ctrl_transfer(BM_IN, GETSTATS, 0, 0, 64))
        return int.from_bytes(buf[26:30], "little") if len(buf) >= 30 else None

    def clkb_verify(self):
        """Read back Si5351 CLK2_CONTROL (bit7 = power-down) — True iff enabled."""
        v = self.i2c_r(SI5351_ADDR, SI_CLK2, 1)[0]
        on = not (v & 0x80)
        print(f"  CLKB verify: CLK2_CTRL=0x{v:02X} -> {'enabled' if on else 'OFF'}")
        return on

    def r828d_probe(self):
        """Read the R828D ID (reg 0x00) in logical order. Returns the byte
        (0x96 expected — the datasheet's logical chip-id), or None if it doesn't
        ACK. _rd bit-reverses the wire 0x69 back to logical 0x96."""
        try:
            v = self._rd(0x00, 1)[0]
        except usb.core.USBError as e:
            print(f"  R828D probe: no I2C response ({e}) — tuner unreachable")
            return None
        print(f"  R828D probe: reg0=0x{v:02X} (want 0x96) -> "
              f"{'OK' if v == 0x96 else 'ID MISMATCH'}")
        return v

    # ── R828D register access (shadowed, like the firmware's priv->regs) ──
    def _wr(self, reg, val):
        self.regs[reg] = val & 0xFF
        self.i2c_w(R828D_ADDR, reg, [val & 0xFF])

    def _wr_mask(self, reg, val, mask):          # read-modify-write vs the shadow
        self._wr(reg, (self.regs.get(reg, 0) & ~mask) | (val & mask))

    def _rd(self, reg, n):
        """Read n R828D registers from reg, in datasheet/librtlsdr LOGICAL bit
        order. The chip streams reads LSB-first so each wire byte is reversed;
        _bitrev8 undoes it. i2c_r stays raw (the Si5351 reads normally), so the
        reverse lives only on this R828D read path."""
        return [_bitrev8(v) for v in self.i2c_r(R828D_ADDR, reg, n)]

    # ── Si5351 CLKB = ref_hz on CLK2/PLL-B (port of si5351aSetFrequencyB) ──
    def clkb_on(self, ref_hz):
        self.ref_hz = ref_hz                          # remember it for set_pll (pll_ref)
        freq, rdiv = ref_hz, 0
        while freq <= 1_000_000:
            freq *= 2; rdiv += 0x10
        divider = 900_000_000 // freq
        if divider % 2: divider -= 1
        pll = divider * freq
        mult = pll // SI5351_XTAL
        num  = ((pll % SI5351_XTAL) * 1048575) // SI5351_XTAL
        denom = 1048575
        self.i2c_w(SI5351_ADDR, SI_PLL_B, self._si_regs(mult, num, denom))
        self.i2c_w(SI5351_ADDR, SI_MS2,   self._si_regs(divider, 0, 1, rdiv))
        self.i2c_w(SI5351_ADDR, SI_PLL_RESET, [0x80])           # reset PLL-B only
        self.i2c_w(SI5351_ADDR, SI_CLK2, [0x4C | 0x20])         # enable CLK2 from PLL-B

    def clkb_off(self):
        self.i2c_w(SI5351_ADDR, SI_CLK2, [0x80])

    @staticmethod
    def _si_regs(a, num, denom, rdiv=None):
        # PLL: a=mult. MS: a=divider, rdiv given. P1/P2/P3 (SetupPLL/SetupMultisynth).
        if rdiv is None:        # PLL
            P1 = 128 * a + (128 * num) // denom - 512
            P2 = 128 * num - denom * ((128 * num) // denom)
            P3 = denom
            d2 = (P1 >> 16) & 0x03
        else:                   # multisynth (integer divider)
            P1 = 128 * a - 512; P2 = 0; P3 = 1
            d2 = ((P1 >> 16) & 0x03) | rdiv
        return [(P3 >> 8) & 0xFF, P3 & 0xFF, d2, (P1 >> 8) & 0xFF, P1 & 0xFF,
                ((P3 >> 12) & 0xF0) | ((P2 >> 16) & 0x0F), (P2 >> 8) & 0xFF, P2 & 0xFF]

    # ── R828D init (port of r82xx_init + set_bandwidth(8 MHz)) ─────────────
    def r828d_init(self):
        for i, v in enumerate(R828D_INIT):           # seed shadow + write 0x05..0x1f
            self.regs[R828D_INIT_BASE + i] = v
            self.i2c_w(R828D_ADDR, R828D_INIT_BASE + i, [v])
        # set_bandwidth(8 MHz): IF channel filter -> IF center 4.57 MHz
        self._wr_mask(0x0A, 0x10, 0x0F)
        self._wr_mask(0x0B, 0x0B, 0xEF)
        self._wr_mask(0x1E, 0x60, 0x40)
        # calibrate filter against the current CLKB reference — must run
        # AFTER the bandwidth preset so the cal code isn't clobbered
        self.calibrate_filter()

    def calibrate_filter(self, park_lo=100_100_000):
        """Port of r82xx_set_tv_standard's IF-filter cal (tuner_r82xx.c:~1300).
        Trims FILT_CODE (0x0A[3:0]) against a reference-derived cal clock, so
        the filter corner tracks CLKB.  Parks the PLL at park_lo — caller must
        retune after.  Returns fil_cal_code (0..15), or None if cal PLL won't
        lock.

        Default 100.1 MHz avoids the two traps that killed 56 MHz:
        - VCO floor (56×32=1792 MHz, barely above 1770 floor)
        - Integer-N (sdm=0 at both 16/32 MHz ref → dither off)
        Callers with a known-good LO can pass it directly."""
        code = 0
        for _ in range(2):
            self._wr_mask(0x0F, 0x04, 0x04)       # cali clk on  (R15 bit 2)
            self._set_mux(int(park_lo))            # sets reg 0x10 xtal/ref bits
            if not self._set_pll(int(park_lo)):    # park PLL for cal clock
                return None
            self._wr_mask(0x0B, 0x10, 0x10)        # start trigger (R11 bit 4)
            self._wr_mask(0x0B, 0x00, 0x10)        # stop trigger
            self._wr_mask(0x0F, 0x00, 0x04)        # cali clk off
            code = self._rd(0x00, 5)[4] & 0x0F     # reg 0x04 low nibble
            if code and code != 0x0F:
                break
        if code == 0x0F:                           # railed -> narrowest fallback
            code = 0
        self._wr_mask(0x0A, 0x10 | code, 0x1F)    # FILT_Q | calibrated code
        print(f"  filter cal: FILT_CODE={code} at ref {self.ref_hz/1e6:.0f} MHz")
        return code

    # ── R828D tune (partial port of set_freq64: set_mux + set_pll + input sw) ─
    def r828d_set_freq(self, rf_hz):
        """LO = RF + IF (low-side). set_mux + set_pll, then the Air-In/Cable1
        input switch. No harmonic retry (out of VHF scope). True iff PLL locked."""
        lo = rf_hz + self.if_hz
        self._set_mux(lo)
        ok = self._set_pll(lo)
        self._wr_mask(0x05, 0x00 if rf_hz > 345_000_000 else 0x60, 0x60)  # Air/Cable
        return ok

    def _set_mux(self, lo):
        mhz = lo // 1_000_000
        band = FREQ_RANGES[0]
        for r in FREQ_RANGES:
            if mhz < r[0]: break
            band = r
        _, open_d, rf_mux_ploy, tf_c = band
        self._wr_mask(0x17, open_d, 0x08)
        self._wr_mask(0x1A, rf_mux_ploy, 0xC3)
        self._wr(0x1B, tf_c)
        self._wr_mask(0x10, 0x08, 0x0B)               # default xtal cap (0pF | drive)

    def _set_pll(self, lo):
        VCO_MIN, VCO_MAX = 1_770_000, 3_540_000       # kHz
        VCO_POWER_REF = 1                             # R828D
        freq_khz = (lo + 500) // 1000
        pll_ref = self.ref_hz                         # the reference we programmed on CLKB
        self._wr_mask(0x10, 0x00, 0x10)               # refdiv2 = 0
        self._wr_mask(0x1A, 0x00, 0x0C)               # PLL autotune 128 kHz
        self._wr_mask(0x12, 0x80, 0xE0)               # VCO current min (0x80)

        mix_div, div_num, found = 2, 0, False
        while mix_div <= 64:
            if VCO_MIN <= freq_khz * mix_div < VCO_MAX:
                found = True
                b = mix_div
                while b > 2:
                    b >>= 1; div_num += 1
                break
            mix_div <<= 1
        if not found:
            print(f"  set_pll: LO {lo/1e6:.4f} MHz out of range "
                  f"(needs VCO {VCO_MIN/1e3:.0f}-{VCO_MAX/1e3:.0f} MHz via mix_div 2..64)")
            return False

        data = self._rd(0x00, 5)
        vco_fine = (data[4] & 0x30) >> 4              # reg 0x04 logical b5:4
        if vco_fine > VCO_POWER_REF: div_num -= 1
        elif vco_fine < VCO_POWER_REF: div_num += 1
        self._wr_mask(0x10, div_num << 5, 0xE0)

        vco_freq = lo * mix_div
        vco_div = (pll_ref + 65536 * vco_freq) // (2 * pll_ref)
        nint, sdm = vco_div // 65536, vco_div % 65536
        if nint > (128 // VCO_POWER_REF) - 1:
            print(f"  set_pll: no valid PLL for {lo/1e6:.4f} MHz LO "
                  f"at ref {pll_ref/1e6:.3f} MHz (nint={nint})")
            return False

        ni = (nint - 13) // 4
        si = nint - 4 * ni - 13
        self._wr(0x14, ni + (si << 6))                # nint
        self._wr_mask(0x12, 0x08 if sdm == 0 else 0x00, 0x18)
        self._wr(0x16, sdm >> 8)                       # sdm hi
        self._wr(0x15, sdm & 0xFF)                     # sdm lo

        time.sleep(0.002)
        locked = False
        for _ in range(2):
            if self._rd(0x00, 3)[2] & 0x40:           # lock = reg 0x02 logical b6 (reads are logical-order)
                locked = True; break
            self._wr_mask(0x12, 0x60, 0xE0)           # bump VCO current to max, retry
        self._wr_mask(0x1A, 0x08, 0x08)               # autotune 8 kHz
        print(f"  LO={lo/1e6:.4f} MHz ref={pll_ref/1e6:.3f} mix_div={mix_div} "
              f"nint={nint} sdm={sdm} lock={'YES' if locked else 'NO'}")
        return locked

    # ── R828D standby (port of r82xx_standby) ─────────────────────────────
    def r828d_standby(self):
        # Old opaque form, kept for reference:
        # for reg, val in ((0x06,0xB1),(0x05,0xA0),(0x07,0x3A),(0x08,0x40),(0x09,0xC0),
        #                  (0x0A,0x36),(0x0C,0x35),(0x0F,0x68),(0x11,0x03),(0x17,0xF4),
        #                  (0x19,0x0C)):
        #     self._wr(reg, val)
        # Power-down sequence: detectors/LNA, mixer, buffers, IF & channel
        # filters, VGA, then the PLL LDOs and RF filter last.
        for reg, val in (
            #(0x06, 0xB1),  # power-detector 1 off, LNA power -> min (R6_FIXED set)
            (0x06, 0xA1),  # ka9q: same but R6_FIXED omitted
            (0x05, 0xA0),  # LNA power DOWN
            (0x07, 0x3A),  # mixer power DOWN (PWD_MIX=0)
            (0x08, 0x40),  # mixer buffer / image-gain amp OFF
            (0x09, 0xC0),  # IF filter / image-phase amp OFF (NOT ka9q 0x41 — that's the 1<7 bug)
            #(0x0A, 0x36),  # channel filter power DOWN (R10_FIXED set)
            (0x0A, 0x26),  # ka9q: R10_FIXED omitted
            #(0x0C, 0x35),  # IF VGA OFF (R12_FIXED set)
            (0x0C, 0x15),  # ka9q: R12_FIXED omitted
            #(0x0F, 0x68),  # clk state (librtlsdr: bit6=1, CLK_OUT_ENB=0, RING_CLK=1)
            (0x0F, 0x30),  # ka9q: CLK_OUT_ENB=1, bit6=0, RING_CLK=0
            (0x11, 0x03),  # PLL analog LDO OFF
            (0x17, 0xF4),  # PLL digital LDO OFF, open-drains safe
            (0x19, 0x0C),  # RF filter OFF, ring-osc clk off
        ):
            self._wr(reg, val)

    # ── Gain control ──────────────────────────────────────────────────────
    def set_lna_gain(self, val):
        """Set LNA gain (reg 0x05 bits [3:0]), range 0-15."""
        self._wr_mask(0x05, val & 0x0F, 0x0F)

    def set_mixer_gain(self, val):
        """Set mixer gain (reg 0x07 bits [3:0]), range 0-15."""
        self._wr_mask(0x07, val & 0x0F, 0x0F)

    def set_vga_gain(self, val):
        """Set IF VGA gain (reg 0x0C bits [3:0]), range 0-15."""
        self._wr_mask(0x0C, val & 0x0F, 0x0F)

    def get_gains(self):
        """Read current gain settings from the register shadow."""
        return {
            'lna':   self.regs.get(0x05, 0) & 0x0F,
            'mixer': self.regs.get(0x07, 0) & 0x0F,
            'vga':   self.regs.get(0x0C, 0) & 0x0F,
        }

    # ── AGC control ───────────────────────────────────────────────────────
    def set_lna_agc(self, on):
        """Set LNA AGC (reg 0x05 bit 4). When on, chip controls LNA gain."""
        self._wr_mask(0x05, 0x10 if on else 0x00, 0x10)

    def set_mixer_agc(self, on):
        """Set mixer AGC (reg 0x07 bit 4). When on, chip controls mixer gain."""
        self._wr_mask(0x07, 0x10 if on else 0x00, 0x10)

    def get_agc(self):
        """Read AGC state from shadow registers."""
        return {
            'lna':   bool(self.regs.get(0x05, 0) & 0x10),
            'mixer': bool(self.regs.get(0x07, 0) & 0x10),
        }

    # ── Channel filter power-down (R10 / 0x0A bit 7) ────────────────────
    def set_chan_filter(self, on):
        """Set channel filter power. on=True: filter active (PWD_FILT=1),
        on=False: filter powered down (PWD_FILT=0).
        The filter is in-line — powering it down disconnects the signal
        path entirely (no signal, not bypass)."""
        self._wr_mask(0x0A, 0x80 if on else 0x00, 0x80)

    def get_chan_filter(self):
        """Return True if channel filter is powered on (PWD_FILT=1)."""
        return bool(self.regs.get(0x0A, 0xDB) & 0x80)

    # ── Filter extension bits ─────────────────────────────────────────────
    def set_filt_ext(self, on):
        """FILTER_EXT — R30/0x1E bit 6. Extends the channel filter skirt.
        Note: set_bandwidth() already writes this bit (1 for 8/7 MHz,
        0 for narrower), so toggling it independently overrides the preset
        until the next bandwidth change."""
        self._wr_mask(0x1E, 0x40 if on else 0x00, 0x40)

    def get_filt_ext(self):
        """Return True if FILTER_EXT (0x1E bit 6) is set."""
        return bool(self.regs.get(0x1E, 0x0E) & 0x40)

    def set_filt_ext_widest(self, on):
        """FLT_EXT_WIDEST — R15/0x0F bit 7. Widens the filter skirt further.
        Never touched by the normal driver — init 0x0F=0x68 leaves it 0."""
        self._wr_mask(0x0F, 0x80 if on else 0x00, 0x80)

    def get_filt_ext_widest(self):
        """Return True if FLT_EXT_WIDEST (0x0F bit 7) is set."""
        return bool(self.regs.get(0x0F, 0x68) & 0x80)

    # ── Bandwidth control ─────────────────────────────────────────────────
    def set_bandwidth(self, bw_mhz):
        """Program the R828D channel filter. Returns the new IF center (Hz).
        8 and 7 MHz keep IF at 4.57 MHz; 6 MHz shifts IF to 3.57 MHz (caller
        should retune the PLL and adjust the ka9q channel frequency)."""
        if bw_mhz not in _BW_PRESETS:
            raise ValueError(f"unsupported bandwidth {bw_mhz} MHz; "
                             f"use one of {BW_CYCLE}")
        r0a, r0b, r1e, if_hz = _BW_PRESETS[bw_mhz]
        self._wr_mask(0x0A, r0a, 0x0F)
        self._wr_mask(0x0B, r0b, 0xEF)
        self._wr_mask(0x1E, r1e, 0x40)
        self.if_hz = if_hz
        return if_hz

    # ── antenna path + lifecycle ──────────────────────────────────────────
    def enter_vhf(self, base, bias_tee):
        word = (base | VHF_EN) & ~BIAS_HF
        if bias_tee: word |= BIAS_VHF
        self.gpio(word)
        return word

    def standby(self, base):
        """Best-effort teardown: attempt every step even if an earlier one
        raises, so a failed R828D standby still turns CLKB off and restores the
        HF GPIO. Prints its own status; returns True iff every step succeeded."""
        errs = []
        for name, step in (
                ("R828D standby", self.r828d_standby),
                ("CLKB off",      self.clkb_off),
                ("HF GPIO",       lambda: self.gpio((base | BIAS_HF) & ~VHF_EN & ~BIAS_VHF))):
            try:
                step()
            except Exception as e:
                errs.append(f"{name}: {e!r}")
        if errs:
            print("  standby PARTIAL — " + "; ".join(errs))
            return False
        print("standby: R828D off, CLKB off, HF")
        return True


def firmware_load(img, fx3_cmd="fx3_cmd", timeout=15):
    """Load firmware if the device is in bootloader (04B4:00F3) by delegating to
    the proven `fx3_cmd load`, then wait for re-enumeration to 04B4:00F1."""
    if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID) is not None:
        print("load: already in app mode (00F1) — skipping"); return
    if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID_BOOT) is None:
        sys.exit("load: device not found in bootloader (00F3) or app mode (00F1)")
    print(f"load: {fx3_cmd} load {img}")
    if subprocess.run([fx3_cmd, "load", img]).returncode != 0:
        sys.exit(f"load: '{fx3_cmd} load' failed")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID) is not None:
            print("load: re-enumerated as 00F1"); return
        time.sleep(0.3)
    sys.exit("load: device did not re-enumerate to app mode (00F1)")
