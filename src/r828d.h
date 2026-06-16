#ifndef _R828D_H
#define _R828D_H 1

// R828 tuner stuff

// Min and Max frequency for VHF/UHF tuner
#define MIN_FREQUENCY  (50.e6)   //  50 MHz
#define MAX_FREQUENCY (2.e9)  // 2000 MHz
#define R828D_REF  (16.e6)        // R828D reference frequency
#define R828D_IF_CARRIER (4.57e6)    // center of IF 4.57 MHz (runs from about 0.6 - 9.4 MHz)
#define R828D_VCO_MIN (1.77e9)
#define R828D_VCO_MAX (2*R828D_VCO_MIN)

// R0
#define R828D_R0_CHIP_ID (0xff)

// R1
#define R828D_R1_FIXED (2<<6)
#define R828D_R1_ADC   (63)

// R2
#define R828D_R2_FIXED (1<<7)
#define R828D_R2_VCO_INDICATOR (1<<6)
#define R828D_R2_ADC   (63)

// R3
#define R828D_R3_RF_INDICATOR (15<<4) // mixer gain
#define R828D_R3_LNA_GAIN (15)

// R4
#define R828D_R4_VCO_FINE_TUNE (3<<4)
#define R828D_R4_FIL_CAL_CODE (15)

// R5
#define R828D_R5_PWD_LT   (1<<7)
#define R828D_R5_PWD_LNA1 (1<<5)
#define R828D_R5_LNA_GAIN_MODE (1<<4)
#define R828D_R5_LNA_GAIN (15)

// R6
#define R828D_R6_PWD_PDET1 (1<<7)
#define R828D_R6_PWD_PDET3 (1<<6)
#define R828D_R6_FILT_3DB  (1<<5)
#define R828D_R6_FIXED     (1<<4)
#define R828D_R6_CABLE2_IN (1<<3)
#define R828D_R6_PW_LNA    (7)

// R7
#define R828D_R7_PWD_MIX   (1<<6)
#define R828D_R7_PW0_MIX   (1<<5)
#define R828D_R7_MIXGAIN_MODE (1<<4)
#define R828D_R7_MIX_GAIN (15)

// R8
#define R828D_R8_PWD_AMP   (1<<7)
#define R828D_R8_PW0_AMP   (1<<6)
#define R828D_R8_I         (1<<5)
#define R828D_R8_IMR_G     (31)

// R9
#define R828D_R9_PWD_IFFILT (1<<7)
#define R828D_R9_PW1_IFFILT (1<<6)
#define R828D_R9_I          (1<<5)
#define R828D_R9_IMR_P      (31)

// R10
#define R828D_R10_PWD_FILT  (1<<7)
#define R828D_R10_PW_FILT   (3<<5)
#define R828D_R10_FIXED     (1<<4)
#define R828D_R10_FILT_CODE (15)

// R11
#define R828D_R11_FILT_BW   (3<<5)
#define R828D_R11_CAL_TRIGGER (1<<4)
#define R828D_R11_HPF       (15)

// R12
// VGA code: 0 = -12 dB, 15 = +40.5 dB, 3.5 dB/step
#define R828D_R12_SW_ADC    (1<<7)
#define R828D_R12_PWD_VGA   (1<<6)
#define R828D_R12_FIXED     (1<<5)
#define R828D_R12_VGA_MODE  (1<<4)
#define R828D_R12_VGA_CODE  (15)

// R13
// 15 -> 1.94V, 0 -> 0.34V, 0.1V/step
#define R828D_R13_LNA_VTHH  (15<<4)
#define R828D_R13_LNA_VTHL  (15)

// R14
// 15 -> 1.94V, 0 -> 0.34V, 0.1V/step
#define R828D_R14_MIX_VTH_H (15<<4)
#define R828D_R14_MIX_VTH_L (15)

// R15
#define R828D_R15_FLT_EXT_WIDEST (1<<7)
#define R828D_R15_FIXED       (1<<5)
#define R828D_R15_CLK_OUT_ENB (1<<4)
#define R828D_R15_RING_CLK    (1<<3) // ring clock for calibration I/Q balance
#define R828D_R15_CALI_CLK    (1<<2)
#define R828D_R15_CLK_AGC_ENB (1<<1)
#define R828D_R15_GPIO        (1<<0)

// R16 0x10
// SEL_DIV = 000 -> mixer in = vco out/2
// SEL_DIV = 101 -> mixer in = vco out/64
#define R828D_R16_SEL_DIV    (7<<5)
// 1 - fref = xtal freq/2
#define R828D_R16_REFDIV     (1<<4)
#define R828D_R16_XTAL       (1<<3)
#define R828D_R16_FIXED      (1<<2)
// 00 = no cap, 01 = 10 pF, 10 = 20 pF, 11 = 30 pF
#define R828D_R16_CAPX       (3)

// R17 0x11
// 00 = 0ff, 01 = 2.1V, 10 = 2.0V, 11 = 1.9V
#define R828D_R17_PW_LDO_A   (3<<6)
// 101: 0.2, 111: auto
#define R828D_R17_CP_CUR     (7<<3)
#define R828D_R17_FIXED      (3)

// R18 0x12
#define R828D_R18_VCOC       (7<<5) // VCO current
#define R828D_R18_DITHER     (1<<4)
#define R828D_R18_PW_SDM     (1<<3)

// R19 0x13
#define R828D_R19_VCOCTL    (1<<6) // vco manual mode
// DAC for VCO: 0 -> min (1.75 GHz) 63 -> max (3.6 GHz)
#define R828D_R19_VCO_DAC   (63)

// R20 0x14
// Nint = 4 * Ni2C + SI2C + 13
// PLL divider Ndiv = (Nint + Nfra) * 2
#define R828D_R20_SI2C       (3<<6)
#define R828D_R20_NI2C       (63)

// R21 0x15
// R21 pll fractional divider input bits 8-1
// R22 0x16
// R22 pll fractional divider input bits 16-9

// R23 0x17
#define R828D_R23_PW_LDO_D   (3<<6)
#define R828D_R23_DIV_BUF_DUR (3<<4)
#define R828D_R23_OPEN_D     (1<<3)
#define R828D_R23_FIXED      (1<<2)

// R24 0x18
#define R828D_R24_FIXED      (1 << 6)
#define R828D_R24_RING_DIV   (1 << 5)
#define R828D_R24_RING_PWR   (1 << 4)
#define R828D_R24_N_RING     (15)

// R25 0x19
#define R828D_R25_PWD_RFFILT (1<<7)
#define R828D_R25_POLFIL_CUR (3<<5)
#define R828D_R25_SW_AGC     (1<<4)
#define R828D_R25_FIXED      (3<<2)
#define R828D_R25_RING_DIV   (3)

// R26 0x1a
#define R828D_R26_RFMUX      (3<<6)
#define R828D_R26_AGC_CLK    (3<<4)
#define R828D_R26_PLL_AUTO_CLK (3<<2)
#define R828D_R26_RFFILT     (3)

// R27 0x1b
// 0000 -> highest corner, 1111 = lowest corner
#define R828D_R27_TF_NCH     (3<<4)
#define R828D_R27_TF_LP      (15)

// R28 0x1c
#define R828D_R28_PDET3_GAIN (15<<4)
#define R828D_R28_DISCG      (1<<3)
#define R828D_R28_FROM_RING  (1<<1)
#define R828D_R28_FIXED      (1<<2)

// R29 0x1d
#define R828D_R29_FIXED      (3<<6)
#define R828D_R29_PDET1_GAIN (7<<3)
#define R828D_R29_PDET2_GAIN (7)

// R30 0x1e
#define R828D_R30_SW_PDECT   (1<<7)
#define R828D_R30_FILTER_EXT (1<<6)
#define R828D_R30_PDET_CLK   (63)

// R31 0x1f
#define R828D_R31_LT_ATT     (1<<7)
#define R828D_R31_FIXED      (1<<6)
#define R828D_R31_PW_RING    (3)


#endif
