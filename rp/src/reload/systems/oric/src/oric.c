// oric.c
//
// ## zlib/libpng license
//
// Copyright (c) 2023 Veselin Sladkov
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the
// use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//     1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software in a
//     product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//     2. Altered source versions must be plainly marked as such, and must not
//     be misrepresented as being the original software.
//     3. This notice may not be removed or altered from any source
//     distribution.

#define CHIPS_IMPL

#define RGBA8(r, g, b) (0xFF000000 | (r << 16) | (g << 8) | (b))

#include <ctype.h>
#include <pico.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chips/chips_common.h"
#include "images/oric_images.h"
#include "pico/stdlib.h"
#ifdef OLIMEX_NEO6502
#include "chips/wdc65C02cpu.h"
#else
#include "chips/mos6502cpu.h"
#endif
#include "chips/ay38910psg.h"
#include "chips/clk.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/mos6522via.h"
#include "debug.h"
#include "devices/disk2_fdc.h"
#include "devices/disk2_fdd.h"
#include "devices/oric_fdc_rom.h"
#include "devices/oric_td.h"
#include "emul.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/ssi.h"
#include "hardware/vreg.h"
#include "kbdmap.h"
#include "oric.h"
#include "pico/multicore.h"
#include "settings/settings.h"

typedef enum {
  ORIC_ROM_LOAD_OK = 0,
  ORIC_ROM_LOAD_ERR_PATH = -1,
  ORIC_ROM_LOAD_ERR_OPEN = -2,
  ORIC_ROM_LOAD_ERR_READ = -3,
  ORIC_ROM_LOAD_ERR_SHORT = -4,
} oric_rom_load_result_t;

typedef struct {
  uint32_t version;
  oric_t oric;
} oric_snapshot_t;

typedef struct {
  oric_t oric;
  uint32_t ticks;
  // double emu_time_ms;
} state_t;

state_t __not_in_flash() state;
uint16_t *oric_via_queue;
uint16_t oric_via_queue_head;
static volatile uint32_t oric_msg_until_us;
static char oric_msg_buf[32];

#ifndef ORIC_MSG_DISPLAY_SECONDS
#define ORIC_MSG_DISPLAY_SECONDS 3u
#endif

static void oric_set_loading_msg(uint8_t fkey) {
  if (fkey < 1 || fkey > 10) {
    return;
  }
  (void)snprintf(oric_msg_buf, sizeof(oric_msg_buf), "Loading F%u file...",
                 (unsigned)fkey);
  oric_msg_until_us = time_us_32() + (ORIC_MSG_DISPLAY_SECONDS * 1000u * 1000u);
}

inline void oric_ayQueuePush(uint16_t *queue, uint16_t *head, uint16_t value) {
  const uint16_t queue_words =
      (uint16_t)(ATARI_ST_VIA_QUEUE_SIZE_BYTES / sizeof(uint16_t));
  uint16_t idx = *head;
  queue[idx] = value;
  uint16_t next_head = (uint16_t)((idx + 1u) & (queue_words - 1u));
  queue[next_head] = 0xFFFF;
  *head = next_head;
}

static inline void flash_set_baud_div(uint16_t div) {
  if (div < 2) div = 2;
  if (div & 1) div++;  // must be even
  ssi_hw->baudr = div;
}

uint8_t __attribute__((section(".oric_rom_in_ram")))
__attribute__((aligned(4))) oric_rom[ORIC_ROM_SIZE] = {0};

// Get oric_desc_t struct based on joystick type
oric_desc_t oric_desc(void) {
  return (oric_desc_t){
      .td_enabled = true,
      .fdc_enabled = true,
      .audio =
          {
              .callback = {.func = NULL},
              .sample_rate = 22050,
          },
      .roms =
          {
              .rom = {.ptr = oric_rom, .size = sizeof(oric_rom)},
              .boot_rom = {.ptr = oric_fdc_rom, .size = sizeof(oric_fdc_rom)},
          },
  };
}

void app_init(void) {
  oric_desc_t desc = oric_desc();
  oric_init(&state.oric, &desc);
}

static int load_oric_rom_from_sd(void) {
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char *folderName = folder ? folder->value : "/oric";
  char path[256];
  size_t name_len = strlen(folderName);
  const char *sep =
      (name_len > 0 && folderName[name_len - 1] == '/') ? "" : "/";
  int path_len = snprintf(path, sizeof(path), "%s%srom.img", folderName, sep);
  if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
    DPRINTF("rom.img path too long\n");
    return ORIC_ROM_LOAD_ERR_PATH;
  }

  FIL file;
  FRESULT res = f_open(&file, path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("Failed to open %s (%d)\n", path, res);
    return ORIC_ROM_LOAD_ERR_OPEN;
  }

  memset(oric_rom, 0, sizeof(oric_rom));
  UINT bytes_read = 0;
  res = f_read(&file, oric_rom, sizeof(oric_rom), &bytes_read);
  f_close(&file);
  if (res != FR_OK) {
    DPRINTF("Failed to read %s (%d)\n", path, res);
    return ORIC_ROM_LOAD_ERR_READ;
  }
  if (bytes_read < sizeof(oric_rom)) {
    DPRINTF("rom.img short read: %u bytes\n", (unsigned)bytes_read);
    return ORIC_ROM_LOAD_ERR_SHORT;
  }
  return ORIC_ROM_LOAD_OK;
}

void __not_in_flash_func(kbd_raw_key_down)(int code) {
  if (isascii(code)) {
    if (isupper(code)) {
      code = tolower(code);
    } else if (islower(code)) {
      code = toupper(code);
    }
  }

  oric_t *sys = &state.oric;

  switch (code) {
    case 0x13A:  // F1
    case 0x13B:  // F2
    case 0x13C:  // F3
    case 0x13D:  // F4
    case 0x13E:  // F5
    case 0x13F:  // F6
    case 0x140:  // F7
    case 0x141:  // F8
    case 0x142:  // F9
    case 0x143:  // F10
    {
      uint8_t index = code - 0x13A;
      oric_set_loading_msg((uint8_t)(index + 1));
      int num_nib_images = CHIPS_ARRAY_SIZE(oric_nib_images);
      if (index < num_nib_images) {
        if (sys->fdc.valid) {
          disk2_fdd_insert_disk(&sys->fdc.fdd[0], oric_nib_images[index]);
        }
      } else {
        index -= num_nib_images;
        if (sys->td.valid) {
          bool inserted = oric_td_insert_tape_sdcard(&sys->td, index);
          if (!inserted) {
            DPRINTF("oric: failed to insert tape image %d\n", index);
          } else {
            DPRINTF("oric: tape image %d inserted\n", index);
          }
        }
      }
      break;
    }

    case 0x144:  // F11
      oric_nmi(sys);
      break;

    case 0x145:  // F12
      oric_reset(sys);
      break;

    default:
      kbd_key_down(&sys->kbd, code);
      break;
  }
}

void __not_in_flash_func(kbd_raw_key_up)(int code) {
  if (isascii(code)) {
    if (isupper(code)) {
      code = tolower(code);
    } else if (islower(code)) {
      code = toupper(code);
    }
  }
  kbd_key_up(&state.oric.kbd, code);
}

void gamepad_state_update(uint8_t index, uint8_t hat_state,
                          uint32_t button_state) {}

void __not_in_flash_func(core1_main()) {
  uint32_t next_update_us = time_us_32();
  while (1) {
    uint32_t now_us = time_us_32();
    if ((int32_t)(now_us - next_update_us) >= 0) {
      uint32_t until_us = oric_msg_until_us;
      if (until_us != 0 && (int32_t)(until_us - now_us) > 0) {
        oric_show_msg(&state.oric, oric_msg_buf);
      } else {
        if (until_us != 0) {
          oric_msg_until_us = 0;
        }
        (void)oric_screen_update(&state.oric);
      }
      next_update_us = now_us + 19968;
    }
  }
  __builtin_unreachable();
}

int __not_in_flash_func(oric_main)() {
  int rom_load_result = load_oric_rom_from_sd();

  // SAFEGUARD START: Init translation table for Oric
  kbdmap_initOric();
  build_oric_pat_lut();

  // SAFEGUARD END

  app_init();

  uint8_t *fb_base = (uint8_t *)&__rom_in_ram_start__;
  oric_via_queue = (uint16_t *)(fb_base + ATARI_ST_VIA_QUEUE_OFFSET);
  oric_via_queue_head = 0;

  uint32_t khz_speed = 260000;

  flash_set_baud_div(khz_speed / 66000);  // Flash at Freq /66MHz
  sleep_us(500);                          // wait for flash to stabilize

  // Set the clock frequency. Keep in mind that if you are managing remote
  // commands you should overclock the CPU to >=225MHz
  bool changed_khz = set_sys_clock_khz(khz_speed, false);
  sleep_us(500);  // wait for clock to stabilize

  // Set the voltage. Be cautios with this. I don't think it's possible to
  // damage the hardware, but it's possible to make the hardware unstable.
  vreg_set_voltage(RP2040_VOLTAGE);
  sleep_us(500);  // wait for voltage to stabilize

#if defined(_DEBUG) && (_DEBUG != 0)
  // Initialize chosen serial port
  stdio_init_all();
  setvbuf(stdout, NULL, _IONBF,
          1);  // specify that the stream should be unbuffered
#endif
  DPRINTF("Changed to %u kHz: %s\n", khz_speed, changed_khz ? "yes" : "no");

  if (rom_load_result != ORIC_ROM_LOAD_OK) {
    DPRINTF("rom.img load error: %d\n", rom_load_result);
    oric_show_msg(&state.oric, "NO ROM FOUND");
    while (1) {
      state.oric.fb_toggle ^= 1u;
      uint8_t *fb_base = (uint8_t *)&__rom_in_ram_start__;
      uint32_t *fb_toggle_fb = (uint32_t *)(fb_base + 0x0FFC);
      *fb_toggle_fb = state.oric.fb_toggle ? 0xFFFFFFFF : 0x0;
      sleep_ms(1000);
    }
  }

  DPRINTF("Core 1 start\n");
  multicore_launch_core1(core1_main);

  uint32_t num_ticks = 19968;
  while (1) {
    uint32_t start_time_in_micros = time_us_32();

    for (uint32_t ticks = 0; ticks < num_ticks; ticks++) {
      oric_tick(&state.oric);
    }

    static bool shift_pressed = false;
    uint16_t addr_value = 0;
    if (emul_addrlog_pop(&addr_value)) {
      if ((addr_value & 0xFFF) == CMD_KEYPRESS ||
          (addr_value & 0xFFF) == CMD_KEYRELEASE) {
        uint16_t key_value = 0;
        if (emul_addrlog_pop(&key_value)) {
          bool is_press = ((addr_value & 0xFFF) == CMD_KEYPRESS);
          uint16_t scan_code = key_value & 0x7F;
          if (kbdmap_isShift(scan_code)) {
            shift_pressed = is_press;
            continue;
          }
          DPRINTF("scan_code: $%02x, %s, shift: %c\n", scan_code,
                  is_press ? "DOWN" : "UP", shift_pressed ? 'Y' : 'N');
          uint16_t ascii_value = kbdmap_StGsx2Ascii(scan_code, shift_pressed);
          if (is_press) {
            kbd_raw_key_down(ascii_value);
          } else {
            kbd_raw_key_up(ascii_value);
          }
        }
      }
    }

    // oric_screen_update(&state.oric);
    kbd_update(&state.oric.kbd, num_ticks);

    uint32_t end_time_in_micros = time_us_32();
    uint32_t execution_time = end_time_in_micros - start_time_in_micros;

    int sleep_time = num_ticks - execution_time;
    if (sleep_time > 0) {
      sleep_us(sleep_time);
    } else {
      DPRINTF("oric: frame overrun by %d us\n", -sleep_time);
    }
  }

  __builtin_unreachable();
}
