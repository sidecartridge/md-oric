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

#define CMD_KEYPRESS 0x0BCD    // Key press
#define CMD_KEYRELEASE 0x0CBA  // Key release
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

// Ring buffer for DMA LSB lookup values.
void __not_in_flash_func(emul_addrlog_clear)(void);
bool __not_in_flash_func(emul_addrlog_pop)(uint16_t *value);
bool __not_in_flash_func(emul_addrlog_peek)(uint16_t *value);
size_t __not_in_flash_func(emul_addrlog_count)(void);

#endif  // EMUL_H
