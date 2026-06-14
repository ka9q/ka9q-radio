#ifndef _R820T_H
#define _R820T_H 1

// R820T/R828 tuner stuff

// Min and Max frequency for VHF/UHF tuner
#define MIN_FREQUENCY  50000000LL   //  50 MHz
#define MAX_FREQUENCY =2000000000LL // 2000 MHz
#define R828D_FREQ  16000000     // R820T reference frequency
#define R828D_IF_CARRIER 4570000 // center of IF 4.57 MHz (runs from about 0.6 - 9.4 MHz)

// R5
#define R820T_PWD_LT (1<<7)
#define R820T_LNA1 (1<<5)
#define R820T_LNA_GAIN_MODE (1<<4)
#define R820T_LNA_GAIN (0xf)

// R6
#define R820T_PWD_PDET1 (1<<7)
#define R820T_PWD_PDET3 (1<<6)
#define R820T_FILT_3DB  (1<<5)
#define R820T_PW_LNA    (7)

// R7
#define R820T_PWD_MIX   (1<<6)
#define R820T_PW0_MIX   (1<<5)
#define R820T_MIXGAIN_MODE (1<<4)
#define R820T_MIX_GAIN (0xf)

// R8
#define R820T_PWD_AMP   (1<<7)
#define R820T_PW0_AMP   (1<<6)
#define R820T_IMR_G     (63)

// R9
#define R820T_PWD_IFFILT (1<7)
#define R820T_PW1_IFFILT (1<<6)
#define R820T_IMR_P      (63)

// R10
#define R820T_PWD_FILT  (1<<7)
#define R820T_PW_FILT   (3<<5)
#define R820T_FILT_CODE (15)

// R11
#define R820T_FILT_BW   (3<<5)
#define R820T_HPF       (15)

// R12
#define R820T_PWD_VGA   (1<<6)
#define R820T_VGA_MODE  (1<<4)
#define R820T_VGA_CODE  (15)

// R13
#define R820T_LNA_VTHH  (15<<4)
#define R820T_LNA_VTHL  (15)

// R14
#define R820T_MIX_VTH_H (15<<4)
#define R820T_MIX_VTH_L (15)

// R15
#define R820T_CLK_OUT_ENB (1<<4)
#define R820T_CLK_AGC_ENB (1<<1)

// R16
#define R820T_SEL_DIV    (7<<5)
#define R820T_REFDIV     (1<<4)
#define R820T_CAPX       (3)

// R17
#define R820T_PW_LDO_A   (3<<6)

// R20
#define R820T_SI2C       (3<<6)
#define R820T_NI2C       (63)

// R21 pll fractional divider input
// R22

// R23
#define R820T_PW_LDO_D   (3<<6)
#define R820T_OPEN_D     (1<<3)

// R25
#define R820T_PWD_RFFILT (1<<7)
#define R820T_SW_AGC     (1<<4)

// R26
#define R820T_RFMUX      (3<<6)
#define R820T_PLL_AUTO_CLK (3<<2)
#define R820T_RFFILT     (3)

// R27




// Reads are bit reversed for some strange reason
static inline uint8_t bitrev(uint8_t b){
  b = ((b & 0xf0) >> 4) | ((b & 0x0f) << 4);
  b = ((b & 0xcc) >> 2) | ((b & 0x33) << 2);
  b = ((b & 0xaa) >> 1) | ((b & 0x55) << 1);
  return b;
}

static inline int r820_read(struct sdrstate *sdr, uint8_t reg, uint8_t *val){
  // Device returns reads LSB first, but writes MSB first (!)
  return bitrev(control_recv(sdr->dev_handle, I2CRFX3, R820_ADDR, reg, val, 1));
}
static inline int r820_write(struct sdrstate *sdr, uint8_t reg, uint8_t *arg, int len){
  return control_send(sdr->dev_handle, I2CWFX3, R820_ADDR, reg, arg, len);
}
static inline int r820_write_byte(struct sdrstate *sdr, uint8_t reg, uint8_t arg){
  return control_send_byte(sdr->dev_handle, I2CWFX3, R820_ADDR, reg, arg);
}
#endif
