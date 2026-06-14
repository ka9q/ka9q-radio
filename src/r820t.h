#ifndef _R820T_H
#define _R820T_H 1

// R820T/R828 tuner stuff

// Min and Max frequency for VHF/UHF tuner
#define MIN_FREQUENCY  (50000000LL)   //  50 MHz
#define MAX_FREQUENCY (2000000000LL)  // 2000 MHz
#define R828D_FREQ  (16000000)        // R820T reference frequency
#define R828D_IF_CARRIER (4570000)    // center of IF 4.57 MHz (runs from about 0.6 - 9.4 MHz)

// R5
#define R820T_R5_PWD_LT   (1<<7)
#define R820T_R5_PWD_LNA1 (1<<5)
#define R820T_R5_LNA_GAIN_MODE (1<<4)
#define R820T_R5_LNA_GAIN (0xf)

// R6
#define R820T_R6_PWD_PDET1 (1<<7)
#define R820T_R6_PWD_PDET3 (1<<6)
#define R820T_R6_FILT_3DB  (1<<5)
#define R820T_R6_PW_LNA    (7)

// R7
#define R820T_R7_PWD_MIX   (1<<6)
#define R820T_R7_PW0_MIX   (1<<5)
#define R820T_R7_MIXGAIN_MODE (1<<4)
#define R820T_R7_MIX_GAIN (0xf)

// R8
#define R820T_R8_PWD_AMP   (1<<7)
#define R820T_R8_PW0_AMP   (1<<6)
#define R820T_R8_IMR_G     (63)

// R9
#define R820T_R9_PWD_IFFILT (1<7)
#define R820T_R9_PW1_IFFILT (1<<6)
#define R820T_R9_IMR_P      (63)

// R10
#define R820T_R10_PWD_FILT  (1<<7)
#define R820T_R10_PW_FILT   (3<<5)
#define R820T_R10_FILT_CODE (15)

// R11
#define R820T_R11_FILT_BW   (3<<5)
#define R820T_R11_HPF       (15)

// R12
#define R820T_R12_PWD_VGA   (1<<6)
#define R820T_R12_VGA_MODE  (1<<4)
#define R820T_R12_VGA_CODE  (15)

// R13
#define R820T_R13_LNA_VTHH  (15<<4)
#define R820T_R13_LNA_VTHL  (15)

// R14
#define R820T_R14_MIX_VTH_H (15<<4)
#define R820T_R14_MIX_VTH_L (15)

// R15
#define R820T_R15_CLK_OUT_ENB (1<<4)
#define R820T_R15_CLK_AGC_ENB (1<<1)

// R16
#define R820T_R16_SEL_DIV    (7<<5)
#define R820T_R16_REFDIV     (1<<4)
#define R820T_R16_CAPX       (3)

// R17
#define R820T_R17_PW_LDO_A   (3<<6)

// R20
#define R820T_R20_SI2C       (3<<6)
#define R820T_R20_NI2C       (63)

// R21 pll fractional divider input
// R22

// R23
#define R820T_R23_PW_LDO_D   (3<<6)
#define R820T_R23_OPEN_D     (1<<3)

// R25
#define R820T_R25_PWD_RFFILT (1<<7)
#define R820T_R25_SW_AGC     (1<<4)

// R26
#define R820T_R26_RFMUX      (3<<6)
#define R820T_R26_PLL_AUTO_CLK (3<<2)
#define R820T_R26_RFFILT     (3)

// R27
#define R820T_R27_TF_NCH     (3<<4)
#define R820T_R27_TF_LP      (15)

// R28
#define R820T_R28_PDET3_GAIN (15<<4)

// R29
#define R820T_R29_PDET1_GAIN (7<<3)
#define R820T_R29_PDET2_GAIN (7)

// R30
#define R820T_R30_PDET_CLK   (63)
#define R820T_R30_FILTER_EXT (1<<6)




#endif
