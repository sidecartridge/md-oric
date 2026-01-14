#pragma once

// oric.h
//
// Oric emulator in a C header.
//
// Do this:
// ~~~C
// #define CHIPS_IMPL
// ~~~
// before you include this file in *one* C or C++ file to create the
// implementation.
//
// Optionally provide the following macros with your own implementation
//
// ~~~C
// CHIPS_ASSERT(c)
// ~~~
//     your own assert macro (default: assert(c))
//
// You need to include the following headers before including oric.h:
//
// - chips/chips_common.h
// - chips/wdc65C02cpu.h | chips/mos6502cpu.h
// - chips/mos6522via.h
// - chips/ay38910psg.h
// - chips/kbd.h
// - chips/mem.h
// - chips/clk.h
// - systems/oric_fdd.h
// - systems/oric_fdc.h
// - systems/oric_fdc_rom.h
// - systems/oric_td.h
//
// ## The Oric
//
//
// TODO!
//
// ## Links
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "chips/chips_common.h"
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
#include "constants.h"
#include "devices/disk2_fdc.h"
#include "devices/oric_td.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bump snapshot version when oric_t memory layout changes
#define ORIC_SNAPSHOT_VERSION (1)

#define ORIC_FREQUENCY (1000000)      // 1 MHz
#define ORIC_MAX_TAPE_SIZE (1 << 16)  // Max size of tape file in bytes

#define ORIC_SCREEN_WIDTH 240   // (240)
#define ORIC_SCREEN_HEIGHT 224  // (224)

#define ORIC_KEY_CTRL (0x146)
#define ORIC_KEY_SHIFT (0x147)

// ROM size (16 KB)
#define ORIC_ROM_SIZE 0x4000u
extern uint8_t oric_rom[ORIC_ROM_SIZE];

// SAFEGUARD START: new framebuffers for the multidevice for Atari ST.
// Number of bitcolors per pixel
#define ATARI_ST_BITCOLORS_PER_PIXEL 3
// Size of the a single framebuffer line in bytes
#define ATARI_ST_FRAMEBUFFER_LINE_SIZE_BYTES \
  (ORIC_SCREEN_WIDTH * ATARI_ST_BITCOLORS_PER_PIXEL / 8)
#define ATARI_ST_FRAMEBUFFER_LINE_SIZE_16WORDS \
  (ATARI_ST_FRAMEBUFFER_LINE_SIZE_BYTES / 2)

// Size of the framebuffer in bytes
#define ATARI_ST_FRAMEBUFFER_SIZE_BYTES \
  (ORIC_SCREEN_HEIGHT * ATARI_ST_FRAMEBUFFER_LINE_SIZE_BYTES)
#define ATARI_ST_FRAMEBUFFER_SIZE_16WORDS (ATARI_ST_FRAMEBUFFER_SIZE_BYTES / 2)
#define ATARI_ST_FRAMEBUFFERS_OFFSET 0x1000
#define ATARI_ST_VIA_QUEUE_SIZE_BYTES 512u
#define ATARI_ST_VIA_QUEUE_OFFSET \
  (ATARI_ST_FRAMEBUFFERS_OFFSET + ATARI_ST_FRAMEBUFFER_SIZE_BYTES)
// SAFEGUARD END

// Config parameters for oric_init()
typedef struct {
  bool td_enabled;   // Set to true to enable tape drive emulation
  bool fdc_enabled;  // Set to true to enable floppy disk controller emulation
  chips_debug_t debug;  // Optional debugging hook
  chips_audio_desc_t audio;
  struct {
    chips_range_t rom;
    chips_range_t boot_rom;
  } roms;
} oric_desc_t;

// Oric emulator state
typedef struct {
  MOS6502CPU_T cpu;
  mos6522via_t via;
  ay38910psg_t psg;
  kbd_t kbd;
  mem_t mem;
  bool valid;
  chips_debug_t debug;

  chips_audio_callback_t audio_callback;

  uint8_t ram[0xC000];
  uint8_t* rom;
  uint8_t* boot_rom;

  int blink_counter;
  uint8_t pattr;

  uint8_t reserved[3];

  // Framebuffer for the Atari ST emulation in the ROM in RAM area
  uint16_t* fb;
  uint16_t fb_toggle;
  // SAFEGUARD END

  volatile bool screen_dirty;

  uint16_t extension;

  oric_td_t td;  // Tape drive

  disk2_fdc_t fdc;  // Disk II floppy disk controller

  uint32_t system_ticks;

} oric_t;

// SAFEGUARD START: LUT for Oric pattern bits
static uint8_t oric_pat_lut[64][6] __attribute__((section(".oric_ram")));
static uint16_t line_buff[120]
    __attribute__((section(".oric_ram")));  // 240 pixels

// SAFEGUARD END

// Oric interface

// Initialize a new Oric instance
void oric_init(oric_t* sys, const oric_desc_t* desc);
// Discard Oric instance
void oric_discard(oric_t* sys);
// Reset a Oric instance
void oric_reset(oric_t* sys);

void oric_tick(oric_t* sys);

int oric_main(void);

// Tick Oric instance for a given number of microseconds, return number of
// executed ticks
uint32_t oric_exec(oric_t* sys, uint32_t micro_seconds);
// Take a snapshot, patches pointers to zero or offsets, returns snapshot
// version
uint32_t oric_save_snapshot(oric_t* sys, oric_t* dst);
// Load a snapshot, returns false if snapshot version doesn't match
bool oric_load_snapshot(oric_t* sys, uint32_t version, oric_t* src);

int __not_in_flash_func(oric_screen_update)(oric_t* sys);
void oric_show_msg(oric_t* sys, const char* msg);
void oric_ayQueuePush(uint16_t* queue, uint16_t* head, uint16_t value);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h> /* memcpy, memset */
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif

extern uint16_t* oric_via_queue;
extern uint16_t oric_via_queue_head;

static void _oric_psg_out(int port_id, uint8_t data, void* user_data);
static uint8_t _oric_psg_in(int port_id, void* user_data);
static void _oric_init_memorymap(oric_t* sys);
static void _oric_init_key_map(oric_t* sys);
static void build_oric_pat_lut(void);
static uint8_t oric_no_rom_glyph_row(char c, int row);

#define PATTR_50HZ (0x02)
#define PATTR_HIRES (0x04)
#define LATTR_ALT (0x01)
#define LATTR_DSIZE (0x02)
#define LATTR_BLINK (0x04)

void oric_init(oric_t* sys, const oric_desc_t* desc) {
  CHIPS_ASSERT(sys && desc);
  if (desc->debug.callback.func) {
    CHIPS_ASSERT(desc->debug.stopped);
  }

  memset(sys, 0, sizeof(oric_t));
  uint8_t* fb_base = (uint8_t*)&__rom_in_ram_start__;
  sys->fb = (uint16_t*)(fb_base + ATARI_ST_FRAMEBUFFERS_OFFSET);
  sys->valid = true;
  sys->debug = desc->debug;
  sys->audio_callback = desc->audio.callback;

  CHIPS_ASSERT(desc->roms.rom.ptr && (desc->roms.rom.size == 0x4000));
  CHIPS_ASSERT(desc->roms.boot_rom.ptr && (desc->roms.boot_rom.size == 0x200));
  sys->rom = desc->roms.rom.ptr;
  sys->boot_rom = desc->roms.boot_rom.ptr;

  MOS6502CPU_INIT(&sys->cpu, &(MOS6502CPU_DESC_T){0});

  mos6522via_init(&sys->via);
  ay38910psg_init(&sys->psg, &(ay38910psg_desc_t){.type = AY38910PSG_TYPE_8912,
                                                  .in_cb = _oric_psg_in,
                                                  .out_cb = _oric_psg_out,
                                                  .magnitude = CHIPS_DEFAULT(
                                                      desc->audio.volume, 1.0f),
                                                  .user_data = sys});

  // setup memory map and keyboard matrix
  _oric_init_memorymap(sys);
  _oric_init_key_map(sys);

  sys->blink_counter = 0;
  sys->pattr = 0;

  sys->extension = 0;

  // Optionally setup tape drive
  if (desc->td_enabled) {
    oric_td_init(&sys->td);
  }

  // Optionally setup floppy disk controller
  if (desc->fdc_enabled) {
    disk2_fdc_init(&sys->fdc);
    if (CHIPS_ARRAY_SIZE(oric_nib_images) > 0) {
      disk2_fdd_insert_disk(&sys->fdc.fdd[0], oric_nib_images[0]);
    }
  }
}

void oric_discard(oric_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  if (sys->fdc.valid) {
    disk2_fdc_discard(&sys->fdc);
  }
  if (sys->td.valid) {
    oric_td_discard(&sys->td);
  }
  sys->valid = false;
}

void oric_nmi(oric_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  MOS6502CPU_NMI(&sys->cpu);
}

void oric_reset(oric_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  mos6522via_reset(&sys->via);
  ay38910psg_reset(&sys->psg);
  if (sys->fdc.valid) {
    disk2_fdc_reset(&sys->fdc);
  }
  if (sys->td.valid) {
    oric_td_reset(&sys->td);
  }
  MOS6502CPU_RESET(&sys->cpu);
}

static void __not_in_flash_func(_oric_mem_rw)(oric_t* sys, uint16_t addr,
                                              bool rw) {
  if ((addr >= 0x0300) && (addr <= 0x03FF)) {
    // Memory-mapped IO area
    if ((addr >= 0x0300) && (addr <= 0x030F)) {
      if (rw) {
        MOS6502CPU_SET_DATA(&sys->cpu, mos6522via_read(&sys->via, addr & 0xF));
      } else {
        mos6522via_write(&sys->via, addr & 0xF, MOS6502CPU_GET_DATA(&sys->cpu));
      }
    } else if ((addr >= 0x0310) && (addr <= 0x031F)) {
      if (sys->fdc.valid) {
        // Disk II FDC
        if (rw) {
          // Memory read
          MOS6502CPU_SET_DATA(&sys->cpu,
                              disk2_fdc_read_byte(&sys->fdc, addr & 0xF));
        } else {
          // Memory write
          disk2_fdc_write_byte(&sys->fdc, addr & 0xF,
                               MOS6502CPU_GET_DATA(&sys->cpu));
        }
      } else {
        if (rw) {
          MOS6502CPU_SET_DATA(&sys->cpu, 0x00);
        }
      }
    } else if ((addr >= 0x0320) && (addr <= 0x03FF)) {
      if (sys->fdc.valid) {
        // Disk II boot rom
        if (rw) {
          // Memory read
          MOS6502CPU_SET_DATA(&sys->cpu,
                              sys->boot_rom[(addr & 0xFF) + sys->extension]);
        } else {
          // SAFEGUARD START: commented out memory mapping switch for the
          // overlay RAM Memory write switch (addr) {
          //   case 0x380:
          //     mem_map_rw(&sys->mem, 0, 0xC000, 0x4000, sys->rom,
          //                sys->overlay_ram);
          //     sys->extension = 0;
          //     break;

          //   case 0x381:
          //     mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->overlay_ram);
          //     sys->extension = 0;
          //     break;

          //   case 0x382:
          //     mem_map_rw(&sys->mem, 0, 0xC000, 0x4000, sys->rom,
          //                sys->overlay_ram);
          //     sys->extension = 0x100;
          //     break;

          //   case 0x383:
          //     mem_map_ram(&sys->mem, 0, 0xC000, 0x4000, sys->overlay_ram);
          //     sys->extension = 0x100;
          //     break;

          //   default:
          //     break;
          // }
          // SAFEGUARD END
        }
      } else {
        if (rw) {
          MOS6502CPU_SET_DATA(&sys->cpu, 0x00);
        }
      }
    }
  } else {
    // Regular memory access
    if (rw) {
      // Memory read
      MOS6502CPU_SET_DATA(&sys->cpu, mem_rd(&sys->mem, addr));
    } else {
      // Memory write
      mem_wr(&sys->mem, addr, MOS6502CPU_GET_DATA(&sys->cpu));

      if (addr >= 0x9800 && addr <= 0xBFDF) {
        sys->screen_dirty = true;
      }
    }
  }
}

static uint8_t _last_motor_state = 0;

void __not_in_flash_func(oric_tick)(oric_t* sys) {
  MOS6502CPU_TICK(&sys->cpu);

  _oric_mem_rw(sys, sys->cpu.addr, sys->cpu.rw);

  // Tick FDC
  if (sys->fdc.valid && (sys->system_ticks & 127) == 0) {
    disk2_fdc_tick(&sys->fdc);
  }

  // Tick VIA
  if ((sys->system_ticks & 3) == 0) {
    MOS6502CPU_SET_IRQ(&sys->cpu, mos6522via_tick(&sys->via, 4));

    // Update PSG state
    if (mos6522via_get_cb2(&sys->via)) {
      const uint8_t psg_data = mos6522via_get_pa(&sys->via);
      if (mos6522via_get_ca2(&sys->via)) {
        ay38910psg_latch_address(&sys->psg, psg_data);
      } else {
        if (sys->psg.addr < 0xe) {
          uint16_t packed =
              (uint16_t)(((uint16_t)sys->psg.addr << 8) | psg_data);
          oric_ayQueuePush(oric_via_queue, &oric_via_queue_head, packed);
        }
        ay38910psg_write(&sys->psg, psg_data);
      }
    }

    if (!mos6522via_get_cb2(&sys->via)) {
      mos6522via_set_pa(&sys->via, ay38910psg_read(&sys->psg));
    }

    // PB0..PB2: select keyboard matrix line
    uint8_t pb = mos6522via_get_pb(&sys->via);
    uint8_t line = pb & 7;
    if (line >= 0 && line <= 7) {
      uint8_t line_mask = 1 << line;
      if (kbd_scan_lines(&sys->kbd) == line_mask) {
        mos6522via_set_pb(&sys->via, pb | (1 << 3));
      } else {
        mos6522via_set_pb(&sys->via, pb & ~(1 << 3));
      }
    }

    if (sys->td.valid) {
      uint8_t motor_state = pb & 0x40;
      if (motor_state != _last_motor_state) {
        if (motor_state) {
          sys->td.port |= ORIC_TD_PORT_MOTOR;
          DPRINTF("oric: motor on\n");
        } else {
          sys->td.port &= ~ORIC_TD_PORT_MOTOR;
          DPRINTF("oric: motor off\n");
        }
        _last_motor_state = motor_state;
      }

      static uint8_t t2 = 0;
      t2++;
      if (t2 == 52) {
        oric_td_tick_sdcard(&sys->td);
        t2 = 0;
      }
      if (sys->td.port & ORIC_TD_PORT_READ) {
        mos6522via_set_cb1(&sys->via, true);
      } else {
        mos6522via_set_cb1(&sys->via, false);
      }
    }
  }

  sys->system_ticks++;
}

// PSG OUT callback (nothing to do here)
static void _oric_psg_out(int port_id, uint8_t data, void* user_data) {
  oric_t* sys = (oric_t*)user_data;
  if (port_id == AY38910PSG_PORT_A) {
    kbd_set_active_columns(&sys->kbd, data ^ 0xFF);
  } else {
    // This shouldn't happen since the AY-3-8912 only has one IO port
  }
}

// PSG IN callback (read keyboard matrix)
static uint8_t _oric_psg_in(int port_id, void* user_data) {
  // this shouldn't be called
  (void)port_id;
  (void)user_data;
  return 0xFF;
}

static void build_oric_pat_lut(void) {
  for (int pat = 0; pat < 64; pat++) {
    for (int b = 0; b < 6; b++) {
      oric_pat_lut[pat][b] = (pat & (0x20 >> b)) ? 1 : 0;
    }
  }
}

static uint8_t oric_no_rom_glyph_row(char c, int row) {
  if (c >= 'a' && c <= 'z') {
    c = (char)(c - 'a' + 'A');
  }
  switch (c) {
    case 'A': {
      static const uint8_t glyph[8] = {0x1E, 0x33, 0x33, 0x3F,
                                       0x33, 0x33, 0x33, 0x00};
      return glyph[row & 7];
    }
    case 'E': {
      static const uint8_t glyph[8] = {0x3F, 0x30, 0x30, 0x3E,
                                       0x30, 0x30, 0x3F, 0x00};
      return glyph[row & 7];
    }
    case 'G': {
      static const uint8_t glyph[8] = {0x1E, 0x33, 0x30, 0x37,
                                       0x33, 0x33, 0x1E, 0x00};
      return glyph[row & 7];
    }
    case 'I': {
      static const uint8_t glyph[8] = {0x3F, 0x0C, 0x0C, 0x0C,
                                       0x0C, 0x0C, 0x3F, 0x00};
      return glyph[row & 7];
    }
    case 'L': {
      static const uint8_t glyph[8] = {0x30, 0x30, 0x30, 0x30,
                                       0x30, 0x30, 0x3F, 0x00};
      return glyph[row & 7];
    }
    case 'N': {
      static const uint8_t glyph[8] = {0x33, 0x3B, 0x37, 0x37,
                                       0x33, 0x33, 0x33, 0x00};
      return glyph[row & 7];
    }
    case 'O': {
      static const uint8_t glyph[8] = {0x1E, 0x33, 0x33, 0x33,
                                       0x33, 0x33, 0x1E, 0x00};
      return glyph[row & 7];
    }
    case 'R': {
      static const uint8_t glyph[8] = {0x3C, 0x33, 0x33, 0x3C,
                                       0x36, 0x33, 0x33, 0x00};
      return glyph[row & 7];
    }
    case 'M': {
      static const uint8_t glyph[8] = {0x33, 0x3F, 0x37, 0x33,
                                       0x33, 0x33, 0x33, 0x00};
      return glyph[row & 7];
    }
    case 'F': {
      static const uint8_t glyph[8] = {0x3F, 0x30, 0x30, 0x3E,
                                       0x30, 0x30, 0x30, 0x00};
      return glyph[row & 7];
    }
    case 'U': {
      static const uint8_t glyph[8] = {0x33, 0x33, 0x33, 0x33,
                                       0x33, 0x33, 0x1E, 0x00};
      return glyph[row & 7];
    }
    case 'D': {
      static const uint8_t glyph[8] = {0x3C, 0x33, 0x33, 0x33,
                                       0x33, 0x33, 0x3C, 0x00};
      return glyph[row & 7];
    }
    case '0': {
      static const uint8_t glyph[8] = {0x1E, 0x33, 0x33, 0x33,
                                       0x33, 0x33, 0x1E, 0x00};
      return glyph[row & 7];
    }
    case '1': {
      static const uint8_t glyph[8] = {0x0C, 0x1C, 0x0C, 0x0C,
                                       0x0C, 0x0C, 0x3F, 0x00};
      return glyph[row & 7];
    }
    case '2': {
      static const uint8_t glyph[8] = {0x1E, 0x33, 0x03, 0x06,
                                       0x0C, 0x18, 0x3F, 0x00};
      return glyph[row & 7];
    }
    case '3': {
      static const uint8_t glyph[8] = {0x1E, 0x33, 0x03, 0x0E,
                                       0x03, 0x33, 0x1E, 0x00};
      return glyph[row & 7];
    }
    case '4': {
      static const uint8_t glyph[8] = {0x06, 0x0E, 0x1E, 0x36,
                                       0x3F, 0x06, 0x06, 0x00};
      return glyph[row & 7];
    }
    case '5': {
      static const uint8_t glyph[8] = {0x3F, 0x30, 0x3E, 0x03,
                                       0x03, 0x33, 0x1E, 0x00};
      return glyph[row & 7];
    }
    case '6': {
      static const uint8_t glyph[8] = {0x0E, 0x18, 0x30, 0x3E,
                                       0x33, 0x33, 0x1E, 0x00};
      return glyph[row & 7];
    }
    case '7': {
      static const uint8_t glyph[8] = {0x3F, 0x03, 0x06, 0x0C,
                                       0x18, 0x18, 0x18, 0x00};
      return glyph[row & 7];
    }
    case '8': {
      static const uint8_t glyph[8] = {0x1E, 0x33, 0x33, 0x1E,
                                       0x33, 0x33, 0x1E, 0x00};
      return glyph[row & 7];
    }
    case '9': {
      static const uint8_t glyph[8] = {0x1E, 0x33, 0x33, 0x1F,
                                       0x03, 0x06, 0x1C, 0x00};
      return glyph[row & 7];
    }
    case '.': {
      static const uint8_t glyph[8] = {0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x0C, 0x0C, 0x00};
      return glyph[row & 7];
    }
    case ' ':
    default:
      return 0x00;
  }
}

void oric_show_msg(oric_t* sys, const char* msg) {
  CHIPS_ASSERT(sys && sys->valid);
  if (!msg || *msg == '\0') {
    return;
  }
  uint16_t* restrict fb = sys->fb;
  memset(fb, 0, ATARI_ST_FRAMEBUFFER_SIZE_16WORDS * sizeof(uint16_t));

  const int glyph_w = 6;
  const int glyph_h = 8;
  const uint8_t fg = 0x07;
  const int len = (int)strlen(msg);
  int start_x = (ORIC_SCREEN_WIDTH - (len * glyph_w)) / 2;
  int start_y = (ORIC_SCREEN_HEIGHT - glyph_h) / 2;
  if (start_x < 0) start_x = 0;
  if (start_y < 0) start_y = 0;

  for (int y = 0; y < glyph_h; y++) {
    int screen_y = start_y + y;
    if (screen_y >= ORIC_SCREEN_HEIGHT) {
      break;
    }
    memset(line_buff, 0, sizeof(line_buff));

    for (int i = 0; i < len; i++) {
      uint8_t row_bits = oric_no_rom_glyph_row(msg[i], y);
      int base_x = start_x + (i * glyph_w);
      for (int bit = 0; bit < glyph_w; bit++) {
        if (row_bits & (1u << (glyph_w - 1 - bit))) {
          int x = base_x + bit;
          if (x < 0 || x >= ORIC_SCREEN_WIDTH) {
            continue;
          }
          int idx = x >> 1;
          uint16_t packed = line_buff[idx];
          if ((x & 1) == 0) {
            packed = (uint16_t)((packed & 0xFFF0u) | fg);
          } else {
            packed = (uint16_t)((packed & 0xF0FFu) | ((uint16_t)fg << 8));
          }
          line_buff[idx] = packed;
        }
      }
    }

    uint16_t* restrict dst_line =
        fb + (screen_y * ATARI_ST_FRAMEBUFFER_LINE_SIZE_16WORDS);
    for (int word = 0; word < 15; word++) {
      uint16_t p0 = 0;
      uint16_t p1 = 0;
      uint16_t p2 = 0;
      uint16_t bit = 0x8000;
      uint16_t* p = dst_line + (word * ATARI_ST_BITCOLORS_PER_PIXEL);
      int base_word = word * 8;

      for (int i = 0; i < 8; i++) {
        uint16_t packed = line_buff[base_word + i];
        uint8_t c0 = packed & 0x0F;
        uint8_t c1 = (packed >> 8) & 0x0F;

        if (c0 & 0x01) p0 |= bit;
        if (c0 & 0x02) p1 |= bit;
        if (c0 & 0x04) p2 |= bit;
        bit >>= 1;

        if (c1 & 0x01) p0 |= bit;
        if (c1 & 0x02) p1 |= bit;
        if (c1 & 0x04) p2 |= bit;
        bit >>= 1;
      }

      p[0] = p0;
      p[1] = p1;
      p[2] = p2;
    }
  }

  sys->fb_toggle ^= 1u;
  uint8_t* fb_base = (uint8_t*)&__rom_in_ram_start__;
  uint32_t* fb_toggle_fb = (uint32_t*)(fb_base + 0x0FFC);
  *fb_toggle_fb = sys->fb_toggle ? 0xFFFFFFFF : 0x0;
  sys->screen_dirty = false;
}

int __not_in_flash_func(oric_screen_update)(oric_t* sys) {
  bool dirty = sys->screen_dirty;
  if (!dirty) return 0;

  bool blink_state = (sys->blink_counter & 0x20) != 0;
  sys->blink_counter = (sys->blink_counter + 1) & 0x3F;

  uint8_t pattr = sys->pattr;
  uint8_t* restrict ram = sys->ram;

  uint16_t* restrict fb = sys->fb;

  for (int y = 0; y < 224; y++) {
    uint16_t* restrict dst_line =
        fb + (y * ATARI_ST_FRAMEBUFFER_LINE_SIZE_16WORDS);

    // Line attributes and current colors
    uint8_t lattr = 0;
    uint8_t fgcol = 7;
    uint8_t bgcol = 0;

    for (int x = 0; x < 40; x++) {
      uint8_t ch, pat;

      if ((pattr & PATTR_HIRES) && y < 200) {
        ch = pat = ram[0xA000 + y * 40 + x];
      } else {
        ch = ram[0xBB80 + (y >> 3) * 40 + x];
        int off = (lattr & LATTR_DSIZE ? y >> 1 : y) & 7;
        const uint8_t* base;

        if (pattr & PATTR_HIRES) {
          base = (lattr & LATTR_ALT) ? (ram + 0x9C00) : (ram + 0x9800);
        } else {
          base = (lattr & LATTR_ALT) ? (ram + 0xB800) : (ram + 0xB400);
        }
        pat = base[((ch & 0x7F) << 3) | off];
      }

      if (!(ch & 0x60)) {
        pat = 0x00;
        switch (ch & 0x18) {
          case 0x00:
            fgcol = ch & 7;
            break;
          case 0x08:
            lattr = ch & 7;
            break;
          case 0x10:
            bgcol = ch & 7;
            break;
          case 0x18:
            pattr = ch & 7;
            break;
        }
      }

      uint8_t c_fg = fgcol;
      uint8_t c_bg = bgcol;

      if (ch & 0x80) {  // inverse
        c_bg ^= 0x07;
        c_fg ^= 0x07;
      }
      if ((lattr & LATTR_BLINK) && blink_state) {
        c_fg = c_bg;
      }

      const uint8_t* bits = oric_pat_lut[pat & 0x3F];
      uint16_t* dst16 = &line_buff[x * 3];
      dst16[0] =
          (uint16_t)((bits[0] ? c_fg : c_bg) | ((bits[1] ? c_fg : c_bg) << 8));
      dst16[1] =
          (uint16_t)((bits[2] ? c_fg : c_bg) | ((bits[3] ? c_fg : c_bg) << 8));
      dst16[2] =
          (uint16_t)((bits[4] ? c_fg : c_bg) | ((bits[5] ? c_fg : c_bg) << 8));
    }

    for (int word = 0; word < 15; word++) {
      uint16_t p0 = 0;
      uint16_t p1 = 0;
      uint16_t p2 = 0;
      uint16_t bit = 0x8000;
      uint16_t* p = dst_line + (word * ATARI_ST_BITCOLORS_PER_PIXEL);
      int base_word = word * 8;

      for (int i = 0; i < 8; i++) {
        uint16_t packed = line_buff[base_word + i];
        uint8_t c0 = packed & 0x0F;
        uint8_t c1 = (packed >> 8) & 0x0F;

        if (c0 & 0x01) p0 |= bit;
        if (c0 & 0x02) p1 |= bit;
        if (c0 & 0x04) p2 |= bit;
        bit >>= 1;

        if (c1 & 0x01) p0 |= bit;
        if (c1 & 0x02) p1 |= bit;
        if (c1 & 0x04) p2 |= bit;
        bit >>= 1;
      }

      p[0] = p0;
      p[1] = p1;
      p[2] = p2;
    }
  }
  sys->pattr = pattr;

  sys->fb_toggle ^= 1u;
  uint8_t* fb_base = (uint8_t*)&__rom_in_ram_start__;
  uint32_t* fb_toggle_fb = (uint32_t*)(fb_base + 0x0FFC);
  *fb_toggle_fb = sys->fb_toggle ? 0xFFFFFFFF : 0x0;

  sys->screen_dirty = false;
  return 1;
}

uint32_t oric_exec(oric_t* sys, uint32_t micro_seconds) {
  CHIPS_ASSERT(sys && sys->valid);
  uint32_t num_ticks = clk_us_to_ticks(ORIC_FREQUENCY, micro_seconds);
  if (0 == sys->debug.callback.func) {
    // run without debug callback
    for (uint32_t ticks = 0; ticks < num_ticks; ticks++) {
      oric_tick(sys);
    }
  } else {
    // run with debug callback
    for (uint32_t ticks = 0; (ticks < num_ticks) && !(*sys->debug.stopped);
         ticks++) {
      oric_tick(sys);
      sys->debug.callback.func(sys->debug.callback.user_data, 0);
    }
  }
  kbd_update(&sys->kbd, micro_seconds);
  oric_screen_update(sys);
  return num_ticks;
}

static void _oric_init_memorymap(oric_t* sys) {
  mem_init(&sys->mem);
  memset(sys->ram, 0, sizeof(sys->ram));
  // SAFEGUARD START: commented out overlay RAM initialization
  // memset(sys->overlay_ram, 0, sizeof(sys->overlay_ram));
  // SAFEGUARD END
  mem_map_ram(&sys->mem, 0, 0x0000, sizeof(sys->ram), sys->ram);
  // SAFEGUARD START: commented out overlay RAM mapping
  // mem_map_rw(&sys->mem, 0, 0xC000, 0x4000, sys->rom, sys->overlay_ram);
  mem_map_rom(&sys->mem, 0, 0xC000, 0x4000, sys->rom);
  // SAFEGUARD END
}

static void _oric_init_key_map(oric_t* sys) {
  kbd_init(&sys->kbd, 2);
  const char* keymap =
      // no shift
      //   01234567 (col)
      "7N5V 1X3"   // row 0
      "JTRF  QD"   // row 1
      "M6B4 Z2C"   // row 2
      "K9;-  \\'"  // row 3
      " <>     "   // row 4
      "UIOP  ]["   // row 5
      "YHGE ASW"   // row 6
      "8L0/   ="   // row 7

      /* shift */
      "&n%v !x#"
      "jtrf  qd"
      "m^b$ z@c"
      "k(:_  |\""
      " ,.     "
      "uiop  }{"
      "yhge asw"
      "*l)?   +";

  CHIPS_ASSERT(strlen(keymap) == 128);
  // shift is column 4, line 4
  kbd_register_modifier(&sys->kbd, 0, 4, 4);
  // ctrl is column 4, line 2
  kbd_register_modifier(&sys->kbd, 1, 4, 2);
  for (int shift = 0; shift < 2; shift++) {
    for (int column = 0; column < 8; column++) {
      for (int line = 0; line < 8; line++) {
        int c = keymap[shift * 64 + line * 8 + column];
        if (c != 0x20) {
          kbd_register_key(&sys->kbd, c, column, line, shift ? (1 << 0) : 0);
        }
      }
    }
  }

  // Special keys
  kbd_register_key(&sys->kbd, 0x20, 0, 4, 0);   // Space
  kbd_register_key(&sys->kbd, 0x150, 5, 4, 0);  // Left
  kbd_register_key(&sys->kbd, 0x14F, 7, 4, 0);  // Right
  kbd_register_key(&sys->kbd, 0x151, 6, 4, 0);  // Down
  kbd_register_key(&sys->kbd, 0x152, 3, 4, 0);  // Up
  kbd_register_key(&sys->kbd, 0x08, 5, 5, 0);   // Delete
  kbd_register_key(&sys->kbd, 0x0D, 5, 7, 0);   // Return
  kbd_register_key(&sys->kbd, ORIC_KEY_CTRL, 4, 2, 0);  // Ctrl
  kbd_register_key(&sys->kbd, ORIC_KEY_SHIFT, 4, 4, 0);  // Shift

  kbd_register_key(&sys->kbd, 0x14, 1, 1, 2);  // Ctrl+T
  kbd_register_key(&sys->kbd, 0x10, 3, 5, 2);  // Ctrl+P
  kbd_register_key(&sys->kbd, 0x06, 3, 1, 2);  // Ctrl+F
  kbd_register_key(&sys->kbd, 0x04, 7, 1, 2);  // Ctrl+D
  kbd_register_key(&sys->kbd, 0x11, 6, 1, 2);  // Ctrl+Q
  kbd_register_key(&sys->kbd, 0x13, 6, 6, 2);  // Ctrl+S
  kbd_register_key(&sys->kbd, 0x0C, 1, 7, 2);  // Ctrl+L
  kbd_register_key(&sys->kbd, 0x0E, 1, 0, 2);  // Ctrl+N
}

void oric_key_up(oric_t* sys, int key_code) {
  CHIPS_ASSERT(sys && sys->valid);
  kbd_key_up(&sys->kbd, key_code);
}

uint32_t oric_save_snapshot(oric_t* sys, oric_t* dst) {
  CHIPS_ASSERT(sys && dst);
  *dst = *sys;
  chips_debug_snapshot_onsave(&dst->debug);
  chips_audio_callback_snapshot_onsave(&dst->audio_callback);
  // m6502_snapshot_onsave(&dst->cpu);
  ay38910psg_snapshot_onsave(&dst->psg);
  oric_td_snapshot_onsave(&dst->td);
  disk2_fdc_snapshot_onsave(&dst->fdc);
  mem_snapshot_onsave(&dst->mem, sys);
  return ORIC_SNAPSHOT_VERSION;
}

bool oric_load_snapshot(oric_t* sys, uint32_t version, oric_t* src) {
  CHIPS_ASSERT(sys && src);
  if (version != ORIC_SNAPSHOT_VERSION) {
    return false;
  }
  static oric_t im;
  im = *src;
  chips_debug_snapshot_onload(&im.debug, &sys->debug);
  chips_audio_callback_snapshot_onload(&im.audio_callback,
                                       &sys->audio_callback);
  // m6502_snapshot_onload(&im.cpu, &sys->cpu);
  ay38910psg_snapshot_onload(&im.psg, &sys->psg);
  oric_td_snapshot_onload(&im.td, &sys->td);
  disk2_fdc_snapshot_onload(&im.fdc, &sys->fdc);
  mem_snapshot_onload(&im.mem, sys);
  *sys = im;
  return true;
}

#endif  // CHIPS_IMPL
