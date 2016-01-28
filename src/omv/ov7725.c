/*
 * This file is part of the OpenMV project.
 * Copyright (c) 2013/2014 Ibrahim Abdelkader <i.abdalkader@gmail.com>
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * OV7725 driver.
 *
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "stm32f4xx_hal.h"
#include "sccb.h"
#include "ov7725.h"
#include "systick.h"
#include "ov7725_regs.h"

#define SVGA_HSIZE     (800)
#define SVGA_VSIZE     (600)

#define UXGA_HSIZE     (1600)
#define UXGA_VSIZE     (1200)

static const uint8_t default_regs[][2] = {
    {COM3,          COM3_SWAP_YUV},
    {COM7,          COM7_RES_VGA | COM7_FMT_RGB565 | COM7_FMT_RGB},

    {COM4,          0x81}, /* PLL */
    {CLKRC,         0x01},

    // VGA Window Size  
    {HSTART,        0x23},
    {HSIZE,         0xA0},
    {VSTART,        0x07},
    {VSIZE,         0xF0},
    {HREF,          0x00},

    // Scale down to QVGA Resoultion 
    {HOUTSIZE,      0x50},
    {VOUTSIZE,      0x78},

    {COM12,         0x03},
    {EXHCH,         0x00},
    {TGT_B,         0x7F},
    {FIXGAIN,       0x09},
    {AWB_CTRL0,     0xE0},
    {DSP_CTRL1,     0xFF},

    {DSP_CTRL2,     DSP_CTRL2_VDCW_EN|DSP_CTRL2_HDCW_EN},

    {DSP_CTRL3,     0x00},
    {DSP_CTRL4,     0x00},
    {COM8,          0xF0},
    {COM6,          0xC5},
    {COM9,          0x11},
    {BDBASE,        0x7F},
    {DBSTEP,        0x03},
    {AEW,           0x40},
    {AEB,           0x30},
    {VPT,           0xA1},
    {EXHCL,         0x00},
    {AWB_CTRL3,     0xAA},
    {COM8,          0xFF},

    //Gamma
    {GAM1,          0x0C},
    {GAM2,          0x16},
    {GAM3,          0x2A},
    {GAM4,          0x4E},
    {GAM5,          0x61},
    {GAM6,          0x6F},
    {GAM7,          0x7B},
    {GAM8,          0x86},
    {GAM9,          0x8E},
    {GAM10,         0x97},
    {GAM11,         0xA4},
    {GAM12,         0xAF},
    {GAM13,         0xC5},
    {GAM14,         0xD7},
    {GAM15,         0xE8},

    {SLOP,          0x20},
    {EDGE1,         0x05},
    {EDGE2,         0x03},
    {EDGE3,         0x00},
    {DNSOFF,        0x01},

    {MTX1,          0xB0},
    {MTX2,          0x9D},
    {MTX3,          0x13},
    {MTX4,          0x16},
    {MTX5,          0x7B},
    {MTX6,          0x91},
    {MTX_CTRL,      0x1E},

    {BRIGHTNESS,    0x08},
    {CONTRAST,      0x20},
    {UVADJ0,        0x81},
    {SDE,           (SDE_CONT_BRIGHT_EN | SDE_SATURATION_EN)},

    // For 30 fps/60Hz
    {DM_LNL,        0x00},
    {BDBASE,        0x7F},
    {DBSTEP,        0x03},

    // Lens Correction, should be tuned with real camera module
    {LC_RADI,       0x10},
    {LC_COEF,       0x10},
    {LC_COEFB,      0x14},
    {LC_COEFR,      0x17},
    {LC_CTR,        0x05},
    {COM5,          0xF5}, //0x65

    {0x00,          0x00},
};

#define NUM_BRIGHTNESS_LEVELS (9)
static const uint8_t brightness_regs[NUM_BRIGHTNESS_LEVELS][2] = {
    {0x38, 0x0e}, /* -4 */
    {0x28, 0x0e}, /* -3 */
    {0x18, 0x0e}, /* -2 */
    {0x08, 0x0e}, /* -1 */
    {0x08, 0x06}, /*  0 */
    {0x18, 0x06}, /* +1 */
    {0x28, 0x06}, /* +2 */
    {0x38, 0x06}, /* +3 */
    {0x48, 0x06}, /* +4 */
};

#define NUM_CONTRAST_LEVELS (9)
static const uint8_t contrast_regs[NUM_CONTRAST_LEVELS][1] = {
    {0x10}, /* -4 */
    {0x14}, /* -3 */
    {0x18}, /* -2 */
    {0x1C}, /* -1 */
    {0x20}, /*  0 */
    {0x24}, /* +1 */
    {0x28}, /* +2 */
    {0x2C}, /* +3 */
    {0x30}, /* +4 */
};

#define NUM_SATURATION_LEVELS (9)
static const uint8_t saturation_regs[NUM_SATURATION_LEVELS][2] = {
    {0x00, 0x00}, /* -4 */
    {0x10, 0x10}, /* -3 */
    {0x20, 0x20}, /* -2 */
    {0x30, 0x30}, /* -1 */
    {0x40, 0x40}, /*  0 */
    {0x50, 0x50}, /* +1 */
    {0x60, 0x60}, /* +2 */
    {0x70, 0x70}, /* +3 */
    {0x80, 0x80}, /* +4 */
};

#include <mp.h>
static int reset()
{
    int i=0;
    const uint8_t (*regs)[2];

    // Reset all registers
    SCCB_Write(COM7, COM7_RESET);

    // Delay 10 ms
    systick_sleep(10);

    // Write default regsiters
    for (i=0, regs = default_regs; regs[i][0]; i++) {
        SCCB_Write(regs[i][0], regs[i][1]);
    }

    return 0;
}

static int set_pixformat(enum sensor_pixformat pixformat)
{
    // Read register COM7
    uint8_t reg = SCCB_Read(COM7);

    switch (pixformat) {
        case PIXFORMAT_RGB565:
            reg =  COM7_SET_FMT(reg, COM7_FMT_RGB);
            break;
        case PIXFORMAT_YUV422:
        case PIXFORMAT_GRAYSCALE:
            reg =  COM7_SET_FMT(reg, COM7_FMT_YUV);
            break;
        default:
            return -1;
    }

    // Write back register COM7
    return SCCB_Write(COM7, reg);
}

static int set_framesize(enum sensor_framesize framesize)
{
    int ret=0;
    uint16_t w=res_width[framesize];
    uint16_t h=res_height[framesize];

    ret |= SCCB_Write(HOUTSIZE, w>>2);
    ret |= SCCB_Write(VOUTSIZE, h>>1);
    return ret;
}

static int set_framerate(enum sensor_framerate framerate)
{
    return 0;
}

static int set_contrast(int level)
{
    int ret=0;

    level += (NUM_CONTRAST_LEVELS / 2);
    if (level < 0 || level >= NUM_CONTRAST_LEVELS) {
        return -1;
    }

    ret |= SCCB_Write(CONTRAST, contrast_regs[level][0]);
    return ret;
}

static int set_brightness(int level)
{
    int ret=0;

    level += (NUM_BRIGHTNESS_LEVELS / 2);
    if (level < 0 || level >= NUM_BRIGHTNESS_LEVELS) {
        return -1;
    }

    ret |= SCCB_Write(BRIGHTNESS, brightness_regs[level][0]);
    ret |= SCCB_Write(SIGN_BIT,   brightness_regs[level][1]);
    return ret;
}

static int set_saturation(int level)
{
    int ret=0;

    level += (NUM_SATURATION_LEVELS / 2 );
    if (level < 0 || level >= NUM_SATURATION_LEVELS) {
        return -1;
    }

    ret |= SCCB_Write(USAT, saturation_regs[level][0]);
    ret |= SCCB_Write(VSAT, saturation_regs[level][1]);
    return ret;
}

static int set_exposure(int exposure)
{
   return 0;
}

static int set_gainceiling(enum sensor_gainceiling gainceiling)
{
    // Read register COM9
    uint8_t reg = SCCB_Read(COM9);

    // Set gain ceiling
    reg = COM9_SET_AGC(reg, gainceiling);

    // Write back register COM9
    return SCCB_Write(COM9, reg);
}

static int set_colorbar(int enable)
{
    // Read register COM3
    uint8_t reg = SCCB_Read(COM3);

    // Set color bar on/off 
    reg = COM3_SET_CBAR(reg, enable);

    // Write back register COM3
    return SCCB_Write(COM3, reg);
}

int ov7725_init(struct sensor_dev *sensor)
{
    /* set function pointers */
    sensor->reset = reset;
    sensor->set_pixformat = set_pixformat;
    sensor->set_framesize = set_framesize;
    sensor->set_framerate = set_framerate;
    sensor->set_contrast  = set_contrast;
    sensor->set_brightness= set_brightness;
    sensor->set_saturation= set_saturation;
    sensor->set_exposure  = set_exposure;
    sensor->set_gainceiling = set_gainceiling;
    sensor->set_colorbar = set_colorbar;

    /* set HSYNC/VSYNC/PCLK polarity */
    sensor->vsync_pol = DCMI_VSPOLARITY_HIGH;
    sensor->hsync_pol = DCMI_HSPOLARITY_LOW;
    sensor->pixck_pol = DCMI_PCKPOLARITY_RISING;

    return 0;
}