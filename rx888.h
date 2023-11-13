#ifndef RX888_H
#define RX888_H

// Copyright (c)  2021 Ruslan Migirov <trapi78@gmail.com>
// Credit: https://github.com/rhgndf/rx888_stream

enum FX3Command {
  // Start GPII engine and stream the data from ADC
  // WRITE: UINT32
  STARTFX3 = 0xAA,

  // Stop GPII engine
  // WRITE: UINT32
  STOPFX3 = 0xAB,

  // Get the information of device
  // including model, version
  // READ: UINT32
  TESTFX3 = 0xAC,

  // Control GPIOs
  // WRITE: UINT32
  GPIOFX3 = 0xAD,

  // Write data to I2c bus
  // WRITE: DATA
  // INDEX: reg
  // VALUE: i2c_addr
  I2CWFX3 = 0xAE,

  // Read data from I2c bus
  // READ: DATA
  // INDEX: reg
  // VALUE: i2c_addr
  I2CRFX3 = 0xAF,

  // Reset USB chip and get back to bootloader mode
  // WRITE: NONE
  RESETFX3 = 0xB1,

  // Set Argument, packet Index/Vaule contains the data
  // WRITE: (Additional Data)
  // INDEX: Argument_index
  // VALUE: arguement value
  SETARGFX3 = 0xB6,

  // Start ADC with the specific frequency
  // Optional, if ADC is running with crystal, this is not needed.
  // WRITE: UINT32 -> adc frequency
  STARTADC = 0xB2,

  // R82XX family Tuner functions
  // Initialize R82XX tuner
  // WRITE: NONE
  TUNERINIT = 0xB4,

  // Tune to a sepcific frequency
  // WRITE: UINT64
  TUNERTUNE = 0xB5,

  // Stop Tuner
  // WRITE: NONE
  TUNERSTDBY = 0xB8,

  // Read Debug string if any
  // READ:
  READINFODEBUG = 0xBA,
};

enum ArgumentList {
    // Set R8xx lna/mixer gain
    // value: 0-29
    R82XX_ATTENUATOR = 1,

    // Set R8xx vga gain
    // value: 0-15
    R82XX_VGA = 2,

    // Set R8xx sideband
    // value: 0/1
    R82XX_SIDEBAND = 3,

    // Set R8xx harmonic
    // value: 0/1
    R82XX_HARMONIC = 4,

    // Set DAT-31 Att
    // Value: 0-63
    DAT31_ATT = 10,

    // Set AD8340 chip vga
    // Value: 0-255
    AD8340_VGA = 11,

    // Preselector
    // Value: 0-2
    PRESELECTOR = 12,

    // VHFATT
    // Value: 0-15
    VHF_ATTENUATOR = 13,
};

#define OUTXIO0 (1U << 0) 	// ATT_LE
#define OUTXIO1 (1U << 1) 	// ATT_CLK
#define OUTXIO2 (1U << 2) 	// ATT_DATA
#define OUTXIO3 (1U << 3)  	// SEL0
#define OUTXIO4 (1U << 4) 	// SEL1
#define OUTXIO5 (1U << 5)  	// SHDWN
#define OUTXIO6 (1U << 6)  	// DITH
#define OUTXIO7 (1U << 7)  	// RAND

#define OUTXIO8 (1U << 8) 	// 256
#define OUTXIO9 (1U << 9) 	// 512
#define OUTXI10 (1U << 10) 	// 1024
#define OUTXI11 (1U << 11)  	// 2048
#define OUTXI12 (1U << 12) 	// 4096
#define OUTXI13 (1U << 13)  	// 8192
#define OUTXI14 (1U << 14)  	// 16384
#define OUTXI15 (1U << 15)  	// 32768
#define OUTXI16 (1U << 16)

enum GPIOPin {
    SHDWN = OUTXIO5,
    DITH = OUTXIO6,
    RANDO = OUTXIO7,
    BIAS_HF = OUTXIO8,
    BIAS_VHF = OUTXIO9,
    LED_YELLOW = OUTXI10,
    LED_RED = OUTXI11,
    LED_BLUE = OUTXI12,
    ATT_SEL0 = OUTXI13,
    ATT_SEL1 = OUTXI14,

    // RX888r2
    VHF_EN = OUTXI15,
    PGA_EN = OUTXI16,
};

static const uint8_t SI5351_ADDR = 0x60 << 1;
static const double SI5351_MAX_VCO_FREQ = 900e6;
static const uint32_t SI5351_MAX_DENOMINATOR = 1048575;

enum SI5351Registers {
  SI5351_REGISTER_PLL_SOURCE   = 15,
  SI5351_REGISTER_CLK_BASE     = 16,
  SI5351_REGISTER_MSNA_BASE    = 26,
  SI5351_REGISTER_MSNB_BASE    = 34,
  SI5351_REGISTER_MS0_BASE     = 42,
  SI5351_REGISTER_MS1_BASE     = 50,
  SI5351_REGISTER_PLL_RESET    = 177,
  SI5351_REGISTER_CRYSTAL_LOAD = 183
};

enum SI5351CrystalLoadValues {
  SI5351_VALUE_CLK_PDN          = 0x80,
  SI5351_VALUE_CRYSTAL_LOAD_6PF = 0x01 << 6 | 0x12,
  SI5351_VALUE_PLLA_RESET       = 0x20,
  SI5351_VALUE_PLLB_RESET       = 0x80,
  SI5351_VALUE_MS_INT           = 0x40,
  SI5351_VALUE_CLK_SRC_MS       = 0x0c,
  SI5351_VALUE_CLK_DRV_8MA      = 0x03,
  SI5351_VALUE_MS_SRC_PLLA      = 0x00,
  SI5351_VALUE_MS_SRC_PLLB      = 0x20
};

#endif
