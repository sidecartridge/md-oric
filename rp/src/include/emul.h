/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205
 * Copyright: 2025 - GOODDATA LABS SL
 * Description: Header for the ROM emulator core and setup features
 */

#ifndef EMUL_H
#define EMUL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"

#define SLEEP_LOOP_MS 100

enum {
  APP_DIRECT = 0,       // Emulation
  APP_MODE_SETUP = 255  // Setup
};

#define APP_MODE_SETUP_STR "255"  // App mode setup string

#define CMD_BOOSTER 0x0DEF     // Booster command

/**
 * @brief
 *
 * Launches the ROM emulator application. Initializes terminal interfaces,
 * configures storage systems, and loads the ROM data from SD or other sources.
 * Manages the main loop which includes firmware bypass,
 * user interaction and potential system resets.
 */
void emul_start();

// Bumped by the DMA IRQ each time the m68k signals "blit finished". Read by
// Core 1 to pace rendering. Deliberately NOT routed through the address ring:
// oric_main pops at most one ring entry per frame, so a per-frame event there
// would starve keyboard input (D-14).
extern volatile uint32_t emul_blitDoneCount;

// Ring buffer for DMA LSB lookup values.
// ROM3 window layout ($FBxxxx, low 16 bits of the sampled address). The
// m68k side keeps the same values as equs in main.s.
#define EMUL_ROM3_KEY_WINDOW 0x8200u   // + IKBD byte (bit 7 = release)
#define EMUL_ROM3_KEY_MASK 0xFF00u
#define EMUL_ROM3_BLITDONE 0x8400u     // one read after each blit

#endif  // EMUL_H
