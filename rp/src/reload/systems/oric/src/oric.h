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
// - devices/oric_microdisc.h
// - devices/oric_td.h
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
#include "devices/oric_microdisc.h"
#include "oric_qr_docs.h"
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
// Frame counter the m68k polls once per VBL to decide whether to re-blit.
// A uint16_t, incremented on every completed frame and free to wrap: the m68k
// only ever tests it for inequality against its own saved copy.
// 16 bits deliberately -- an m68k `move.l` is two word reads, so a 32-bit value
// written natively by the RP would reach the ST with its halfwords swapped. A
// 16-bit value crosses the bus intact (the PIO's DMA does a halfword load).
// Bytes 0x0FFE-0x0FFF are unused padding up to the framebuffer at 0x1000.
// RP->m68k command longword, polled once per VBL by the cart code. Sits past
// the ~1.3 KB image but inside the $1000 copied to ST RAM, so the poll works
// from the copy. Must match LISTENER_ADDR / REMOTE_RESET in main.s.
#define ATARI_ST_LISTENER_OFFSET 0x05F8
#define ATARI_ST_REMOTE_RESET 1u

// RP->m68k boot status, read once by the cartridge before it commits to
// running the emulator. Without a card there is no ROM, so the cart code
// prints a line and returns to GEM -- the same exit as the resolution check.
// 16-bit, so the bus byte-swap within a word is transparent (as for the
// frame counter). Must match BOOTSTATUS_ADDR / BOOT_NO_SDCARD in main.s.
#define ATARI_ST_BOOTSTATUS_OFFSET 0x05FC
#define ATARI_ST_BOOT_OK 0u
#define ATARI_ST_BOOT_NO_SDCARD 1u

// The cart bus swaps bytes within each 16-bit word, which makes uint16_t
// transparent -- but an m68k move.l is two word reads in (high, low) order
// while the halves stay in their RP positions, so a uint32_t arrives with its
// halves swapped. Store exact-value longwords half-swapped. Same rule as
// cart_asM68kLong() in md-framebuffer-template's cart_shared.h.
static inline uint32_t _oric_as_m68k_long(uint32_t v) {
  return (v << 16) | (v >> 16);
}

#define ATARI_ST_FRAME_COUNTER_OFFSET 0x0FFCu
// The two framebuffers. B sits in the upper half of the 64 KB window, freed by
// moving oric_rom into ORIC_RAM. The window is linear: the PIO builds the read
// address as 0x20030000 | addr16, so $8000+ is backed by ORIC_ROM_IN_RAM.
#define ATARI_ST_FRAMEBUFFER_A_OFFSET ATARI_ST_FRAMEBUFFERS_OFFSET
#define ATARI_ST_FRAMEBUFFER_B_OFFSET 0x8000u
#define ATARI_ST_VIA_QUEUE_SIZE_BYTES 512u
#define ATARI_ST_VIA_QUEUE_OFFSET \
  (ATARI_ST_FRAMEBUFFERS_OFFSET + ATARI_ST_FRAMEBUFFER_SIZE_BYTES)
// SAFEGUARD END

// Names accepted for the Microdisc EPROM in the content folder (D-17). The
// ROM picker skips them; the file's presence is what makes the controller
// exist. Two spellings because the copies in circulation disagree: our docs
// say microdisc.rom, while Oricutron and the MiSTer core ship the 8.3 name
// MICRODIS.ROM. Matching is case-insensitive, so only the stems differ.
#define ORIC_MICRODISC_ROM_NAME "microdisc.rom"
#define ORIC_MICRODISC_ROM_ALT "microdis.rom"

// Config parameters for oric_init()
typedef struct {
  bool td_enabled;      // Set to true to enable tape drive emulation
  chips_debug_t debug;  // Optional debugging hook
  chips_audio_desc_t audio;
  struct {
    chips_range_t rom;
    chips_range_t microdisc_rom;  // 8 KB; size 0 = no Microdisc fitted
  } roms;
  uint8_t* overlay_ram;  // 16 KB under the BASIC ROM, used while ROMDIS
  uint8_t* md_track;     // ORIC_MD_TRACK_BYTES, the one resident disk track
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

  int blink_counter;
  uint8_t pattr;

  uint8_t reserved[3];

  // Framebuffer for the Atari ST emulation in the ROM in RAM area
  uint16_t* fb;
  uint16_t fb_frame_counter;
  // SAFEGUARD END

  volatile bool screen_dirty;

  // A video mode the CPU wrote but the frame scan may never see. Core 0
  // sets these, Core 1 consumes them at the start of a frame.
  volatile uint8_t pattr_pending;
  volatile bool pattr_pending_valid;

  oric_td_t td;  // Tape drive

  // Microdisc floppy controller (EPIC-07). md_present is false when no
  // microdisc.rom was found; the registers then read as they did before.
  bool md_present;
  oric_wd17xx_t wd;
  oric_microdisc_t md;
  oric_diskimage_t disk;
  uint8_t* overlay_ram;
  const uint8_t* md_rom;

  uint32_t system_ticks;

} oric_t;

// SAFEGUARD START: LUT for Oric pattern bits
static uint8_t oric_pat_lut[64][6] __attribute__((section(".oric_ram")));
// One palette index per pixel: the chunked layout md-framebuffer-template's
// transpose expects. 4-byte aligned so the packer can read it as uint32.
static uint8_t line_buff[240] __attribute__((section(".oric_ram")))
__attribute__((aligned(4)));

// SAFEGUARD END

// Oric interface

// Initialize a new Oric instance
void oric_init(oric_t* sys, const oric_desc_t* desc);
// Discard Oric instance
void oric_discard(oric_t* sys);
// Reset a Oric instance
void oric_reset(oric_t* sys);
// Power-cycle semantics: RAM cleared and re-patterned, then reset. A soft
// reset keeps RAM, and the Atmos ROM then does a warm start that preserves
// whatever page-2 hooks a DOS left behind -- pointing into RAM that is now
// hidden behind the BASIC ROM, so the tape routines never come back.
void oric_cold_reset(oric_t* sys);

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

// Character-cell overlay. The Oric screen is 240x224 and font8x8 is an 8x8
// cell, so the overlay is exactly 30 columns x 28 rows. Cells rather than a
// byte-per-pixel buffer because every menu screen is text on a grid: 30x28
// costs 1680 bytes against the 53760 a full chunked buffer would need, which
// does not fit the RAM budget.
//
// Draw with clear/text/fill, then present() once -- it renders the cells into
// the framebuffer and publishes the frame. Nothing is drawn per emulator frame:
// the ST keeps showing the last published frame until the counter moves, so a
// static menu costs nothing.
#define ORIC_OVL_COLS 30
#define ORIC_OVL_ROWS 28
#define ORIC_OVL_ATTR(fg, bg) ((uint8_t)(((fg) & 7u) | (((bg) & 7u) << 4)))

void oric_ovl_clear(uint8_t attr);
void oric_ovl_text(int col, int row, const char* str, uint8_t attr);
void oric_ovl_fill(int col, int row, int ncols, uint8_t attr);
void oric_ovl_present(oric_t* sys);
// Overlay the docs QR code on the current frame, centred horizontally with
// its top at `y0`. Call it after oric_ovl_present().
void oric_ovl_qr(oric_t* sys, int y0);
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
static uint8_t oric_glyph_row(char c, int row);

#define PATTR_50HZ (0x02)
#define PATTR_HIRES (0x04)
#define LATTR_ALT (0x01)
#define LATTR_DSIZE (0x02)
#define LATTR_BLINK (0x04)

// Apply the Microdisc's ROMDIS / EPROM lines to the page table. Upstream
// tests the flags on every access (machine.c microdisc_atmosread/write); here
// the map changes only when a $314 write flips them, which is rare.
//   romdis clear : $C000-$FFFF is the BASIC ROM, writes dropped
//   romdis set   : $C000-$FFFF is overlay RAM, except that while diskrom is
//                  set the EPROM reads at $E000-$FFFF (writes there dropped)
// Power-on contents of the RAM under the ROM matter. Sedoric's boot loader
// checksums $C980-$FFFF and, if the sum is zero, loads only four sectors
// instead of the whole OS -- then runs off the end into zeros and BRKs
// forever at its own IRQ vector. Real DRAM is never all-zero at power-on; a
// memset(0) buffer is, and reproduces that hang exactly (verified in
// Oricutron with its RAM fill zeroed). Use the fill Oricutron gives an
// Atmos: 128 x 00 then 128 x FF in every page.
static void _oric_overlay_power_on(oric_t* sys) {
  if (!sys->md_present) {
    return;
  }
  for (int i = 0; i < 0x4000; i += 256) {
    memset(sys->overlay_ram + i, 0x00, 128);
    memset(sys->overlay_ram + i + 128, 0xFF, 128);
  }
}

// Would the ULA scan this address in the mode it is in now? Text reads the
// text screen; hires reads the bitmap plus the three text rows below it.
static inline bool _oric_ula_scans(const oric_t* sys, uint16_t addr) {
  if (sys->pattr & PATTR_HIRES) {
    return (addr >= 0xA000 && addr < 0xBF40) ||
           (addr >= 0xBF68 && addr <= 0xBFDF);
  }
  return addr >= 0xBB80 && addr <= 0xBFDF;
}

static void _oric_md_remap(oric_t* sys) {
  if (!sys->md_present || !sys->md.romdis) {
    mem_map_rom(&sys->mem, 0, 0xC000, 0x4000, sys->rom);
    return;
  }
  mem_map_ram(&sys->mem, 0, 0xC000, 0x2000, sys->overlay_ram);
  if (sys->md.diskrom) {
    mem_map_rom(&sys->mem, 0, 0xE000, 0x2000, sys->md_rom);
  } else {
    mem_map_ram(&sys->mem, 0, 0xE000, 0x2000, sys->overlay_ram + 0x2000);
  }
}

void oric_init(oric_t* sys, const oric_desc_t* desc) {
  CHIPS_ASSERT(sys && desc);
  if (desc->debug.callback.func) {
    CHIPS_ASSERT(desc->debug.stopped);
  }

  memset(sys, 0, sizeof(oric_t));
  uint8_t* fb_base = (uint8_t*)&__rom_in_ram_start__;
  // Each render re-targets sys->fb from the counter (see _oric_fb_for_count);
  // this is only the value before the first frame.
  sys->fb = (uint16_t*)(fb_base + ATARI_ST_FRAMEBUFFER_A_OFFSET);
  sys->valid = true;
  sys->debug = desc->debug;
  sys->audio_callback = desc->audio.callback;

  CHIPS_ASSERT(desc->roms.rom.ptr && (desc->roms.rom.size == 0x4000));
  sys->rom = desc->roms.rom.ptr;

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
  sys->pattr_pending = 0;
  sys->pattr_pending_valid = false;

  // Optionally setup tape drive
  if (desc->td_enabled) {
    oric_td_init(&sys->td);
  }

  // Microdisc, present only with its EPROM (D-17)
  sys->md_present = desc->roms.microdisc_rom.ptr &&
                    (desc->roms.microdisc_rom.size == ORIC_MD_ROM_BYTES) &&
                    desc->overlay_ram && desc->md_track;
  sys->md_rom = desc->roms.microdisc_rom.ptr;
  sys->overlay_ram = desc->overlay_ram;
  memset(&sys->disk, 0, sizeof(sys->disk));
  sys->disk.track = desc->md_track;
  sys->disk.buf_track = -1;
  sys->disk.buf_side = -1;
  sys->disk.cachedtrack = -1;
  sys->disk.cachedside = -1;
  microdisc_init(&sys->md, &sys->wd);
  sys->wd.disk[0] = &sys->disk;
  _oric_overlay_power_on(sys);
  _oric_md_remap(sys);
}

void oric_discard(oric_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  diskimage_close(&sys->disk);
  if (sys->td.valid) {
    oric_td_discard(&sys->td);
  }
  sys->valid = false;
}

void oric_nmi(oric_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  MOS6502CPU_NMI(&sys->cpu);
}

void oric_cold_reset(oric_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  memset(sys->ram, 0, sizeof(sys->ram));
  _oric_overlay_power_on(sys);
  oric_reset(sys);
}

void oric_reset(oric_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  mos6522via_reset(&sys->via);
  ay38910psg_reset(&sys->psg);
  if (sys->td.valid) {
    oric_td_reset(&sys->td);
  }
  // Microdisc: a real one holds ROMDIS at power-on so its EPROM boots the
  // machine -- and with no disk in the drive it then just sits there. D-17:
  // assert ROMDIS only when a disk is inserted, so a disk-less power-on still
  // lands in BASIC exactly as before.
  microdisc_init(&sys->md, &sys->wd);
  sys->wd.disk[0] = &sys->disk;
  sys->md.romdis = sys->md_present && sys->disk.inserted;
  _oric_md_remap(sys);
  // Let go of every key the matrix thinks is held. A stuck slot is an
  // emulator artefact, not machine state, and a reset is where a user
  // expects it to clear.
  for (int i = 0; i < KBD_MAX_PRESSED_KEYS; i++) {
    sys->kbd.key_buffer[i] = (key_state_t){0};
  }
  kbd_update(&sys->kbd, 0);
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
      // Microdisc: $310-$313 WD1793, $314 control/status, $318 DRQ. The
      // rest of the range is the VIA again, as upstream's default branch
      // did. A $314 write can move ROMDIS / the EPROM, so remap after it.
      if (sys->md_present && (addr < 0x0315 || addr == 0x0318)) {
        if (rw) {
          MOS6502CPU_SET_DATA(&sys->cpu, microdisc_read(&sys->md, addr));
        } else {
          const bool romdis = sys->md.romdis;
          const bool diskrom = sys->md.diskrom;
          microdisc_write(&sys->md, addr, MOS6502CPU_GET_DATA(&sys->cpu));
          if (romdis != sys->md.romdis || diskrom != sys->md.diskrom) {
            _oric_md_remap(sys);
          }
        }
      } else if (sys->md_present) {
        if (rw) {
          MOS6502CPU_SET_DATA(&sys->cpu,
                              mos6522via_read(&sys->via, addr & 0xF));
        } else {
          mos6522via_write(&sys->via, addr & 0xF,
                           MOS6502CPU_GET_DATA(&sys->cpu));
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
        // A mode attribute takes effect when the ULA scans it, which on real
        // hardware is within the same frame as the write. Core 1 renders a
        // whole-frame snapshot instead, so an attribute the program
        // overwrites straight away is never seen -- and "switch to HIRES,
        // then fill $A000-$BF3F" does exactly that, because $BB80 is inside
        // the fill. The mode would be lost for good. Latch it here, on the
        // write. Attributes that stay in memory still come from the frame
        // scan, so a mid-screen mode change is unaffected.
        const uint8_t v = MOS6502CPU_GET_DATA(&sys->cpu);
        if ((v & 0x78) == 0x18 && _oric_ula_scans(sys, addr)) {
          sys->pattr_pending = v & 7;
          sys->pattr_pending_valid = true;
        }
      }
    }
  }
}

static uint8_t _last_motor_state = 0;

void __not_in_flash_func(oric_tick)(oric_t* sys) {
  MOS6502CPU_TICK(&sys->cpu);

  _oric_mem_rw(sys, sys->cpu.addr, sys->cpu.rw);

  // WD1793 delayed INTRQ / DRQ. Only the two counters are touched per cycle;
  // the command state machine runs on register access.
  if (sys->wd.delayedint > 0 || sys->wd.delayeddrq > 0) {
    wd17xx_ticktock(&sys->wd, 1);
  }

  // Tick VIA. The Microdisc's IRQ (already gated by INTENA) shares the line.
  if ((sys->system_ticks & 3) == 0) {
    MOS6502CPU_SET_IRQ(&sys->cpu,
                       mos6522via_tick(&sys->via, 4) || sys->md.irq);

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

// Glyph lookup into the shared 8x8 ASCII strike (font8x8.h, ported from
// md-framebuffer-template). Returns the row's bits with **bit 0 = leftmost
// pixel**, which is that asset's contract; unsupported codepoints render blank.
static uint8_t oric_glyph_row(char c, int row) {
  const struct FB_FONT* f = &font8x8;
  unsigned char ch = (unsigned char)c;
  if (ch < (unsigned)f->first_char ||
      ch >= (unsigned)(f->first_char + f->num_chars) || row < 0 ||
      row >= f->h) {
    return 0x00;
  }
  return f->data[(ch - f->first_char) * f->h + row];
}

// A frame is rendered into the buffer named by bit 0 of the counter value it
// will publish. The m68k reads that same counter and picks the same buffer
// with `btst #0`, so the two sides agree without any extra shared field.
static inline uint16_t* _oric_fb_for_count(uint16_t count) {
  uint8_t* fb_base = (uint8_t*)&__rom_in_ram_start__;
  return (uint16_t*)(fb_base + ((count & 1u) ? ATARI_ST_FRAMEBUFFER_B_OFFSET
                                             : ATARI_ST_FRAMEBUFFER_A_OFFSET));
}

// Pack 240 palette-index bytes into 15 ST planar word-triples.
//
// Multiply-comb transpose, ported from md-framebuffer-template's
// `fb_c2p_half` (rp/src/fb_chunked_asm.S). Per 4 pixels held in one uint32:
//
//     nibble_K = (((q >> K) & 0x01010101) * 0x80402010) >> 28
//
// The AND isolates bit K of each byte; the multiply scatters those four bits
// into the product's top nibble, leftmost pixel in the MSB -- which is the ST
// shifter's convention. Three planes here rather than the template's four.
//
// Replaces six conditional bit-sets per two pixels: on a Cortex-M0+ with no
// conditional execution those were ~161k real branches per frame.
// Proven bit-identical to the loop it replaces over every single-pixel case
// and 200k random lines.
static inline void __not_in_flash_func(_oric_pack_line)(
    uint16_t* restrict dst, const uint8_t* restrict src) {
  for (int word = 0; word < 15; word++) {
    uint32_t p0 = 0;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    for (int g = 0; g < 4; g++) {
      uint32_t q;
      __builtin_memcpy(&q, src, sizeof(q));
      src += 4;
      const int sh = 12 - 4 * g;
      p0 |= ((((q >> 0) & 0x01010101u) * 0x80402010u) >> 28) << sh;
      p1 |= ((((q >> 1) & 0x01010101u) * 0x80402010u) >> 28) << sh;
      p2 |= ((((q >> 2) & 0x01010101u) * 0x80402010u) >> 28) << sh;
    }
    dst[0] = (uint16_t)p0;
    dst[1] = (uint16_t)p1;
    dst[2] = (uint16_t)p2;
    dst += ATARI_ST_BITCOLORS_PER_PIXEL;
  }
}

// Cell storage: one character byte and one attribute byte per cell.
// Attribute packs fg in bits 0-2 and bg in bits 4-6 (Oric palette is 0-7).
static uint8_t ovl_ch[ORIC_OVL_COLS * ORIC_OVL_ROWS]
    __attribute__((section(".oric_ram")));
static uint8_t ovl_at[ORIC_OVL_COLS * ORIC_OVL_ROWS]
    __attribute__((section(".oric_ram")));

void oric_ovl_clear(uint8_t attr) {
  memset(ovl_ch, ' ', sizeof(ovl_ch));
  memset(ovl_at, attr, sizeof(ovl_at));
}

void oric_ovl_text(int col, int row, const char* str, uint8_t attr) {
  if (!str || row < 0 || row >= ORIC_OVL_ROWS) {
    return;
  }
  for (int c = col; *str; str++, c++) {
    if (c < 0) {
      continue;
    }
    if (c >= ORIC_OVL_COLS) {
      break;
    }
    ovl_ch[row * ORIC_OVL_COLS + c] = (uint8_t)*str;
    ovl_at[row * ORIC_OVL_COLS + c] = attr;
  }
}

// Recolour a run of cells without touching their characters -- the selection
// highlight bar in md-gpu-demo's menu, done with attributes instead of a rect.
void oric_ovl_fill(int col, int row, int ncols, uint8_t attr) {
  if (row < 0 || row >= ORIC_OVL_ROWS) {
    return;
  }
  for (int c = col; c < col + ncols; c++) {
    if (c < 0 || c >= ORIC_OVL_COLS) {
      continue;
    }
    ovl_at[row * ORIC_OVL_COLS + c] = attr;
  }
}

void __not_in_flash_func(oric_ovl_present)(oric_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  uint16_t next_count = (uint16_t)(sys->fb_frame_counter + 1u);
  uint16_t* restrict fb = _oric_fb_for_count(next_count);
  sys->fb = fb;

  for (int py = 0; py < ORIC_SCREEN_HEIGHT; py++) {
    const int crow = py >> 3;
    const int grow = py & 7;
    const uint8_t* restrict ch = &ovl_ch[crow * ORIC_OVL_COLS];
    const uint8_t* restrict at = &ovl_at[crow * ORIC_OVL_COLS];
    uint8_t* restrict out = line_buff;
    for (int c = 0; c < ORIC_OVL_COLS; c++) {
      const uint8_t bits = oric_glyph_row((char)ch[c], grow);
      const uint8_t fg = at[c] & 7u;
      const uint8_t bg = (uint8_t)((at[c] >> 4) & 7u);
      // font8x8 rows are LSB-left: bit 0 is the leftmost pixel.
      for (int b = 0; b < 8; b++) {
        *out++ = (bits & (1u << b)) ? fg : bg;
      }
    }
    _oric_pack_line(fb + (py * ATARI_ST_FRAMEBUFFER_LINE_SIZE_16WORDS),
                    line_buff);
  }

  sys->fb_frame_counter = next_count;
  uint8_t* fb_base = (uint8_t*)&__rom_in_ram_start__;
  uint16_t* fb_counter = (uint16_t*)(fb_base + ATARI_ST_FRAME_COUNTER_OFFSET);
  *fb_counter = next_count;
  // Deliberately does NOT clear screen_dirty. The overlay did not render the
  // Oric screen, it overwrote it -- so any pending dirty state must survive,
  // or the Oric screen never comes back once the overlay goes away. That is
  // invisible while the Oric is writing to screen RAM every frame, and very
  // visible during a tape load, when it writes nothing for seconds at a time.
}

void oric_show_msg(oric_t* sys, const char* msg) {
  CHIPS_ASSERT(sys && sys->valid);
  if (!msg || *msg == '\0') {
    return;
  }
  // A centred single line, now expressed through the overlay primitives rather
  // than its own glyph loop. Keeping it on the shared path means the loading
  // message doubles as the smoke test for the cell overlay.
  const uint8_t attr = ORIC_OVL_ATTR(7, 0);
  oric_ovl_clear(attr);
  int len = (int)strlen(msg);
  int col = (ORIC_OVL_COLS - len) / 2;
  if (col < 0) {
    col = 0;
  }
  oric_ovl_text(col, ORIC_OVL_ROWS / 2, msg, attr);
  oric_ovl_present(sys);
}

// ---------------------------------------------------------------------------
// Tape loading band. The bottom rows of the Oric screen show a progress bar
// while a tape is playing, then "TAPE FINISHED" briefly when it ends.
//
// Real progress rather than an animation: it separates loading, stalled and
// finished, which a spinner cannot. Written by Core 1 from the tape drive's
// own position, so it costs a couple of rows out of 224 and only while the
// motor is running.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Boot hint. "Press F1 for config menu" over the middle of the Oric screen
// while the machine starts, so the menu is discoverable without the README.
// Drawn into the framebuffer after the conversion, like the tape band, so the
// Oric screen underneath is untouched.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Docs QR code. Drawn into the framebuffer like the tape band, straight from
// the pre-computed bitmap -- dark modules on a light field, with the quiet
// zone the spec requires, because a code without it will not scan.
// ---------------------------------------------------------------------------
#define ORIC_QR_QUIET 4  // modules of light border, per the QR spec
#define ORIC_QR_SCALE 2  // screen pixels per module
#define ORIC_QR_SIDE_PX \
  ((ORIC_QR_MODULES + 2 * ORIC_QR_QUIET) * ORIC_QR_SCALE)

// Draw the code with its top-left at (x0, y0). Everything else on those
// scanlines is blacked out: _oric_pack_line rewrites a whole line, so the
// caller must place this where no text is.
static void __not_in_flash_func(_oric_draw_qr)(uint16_t* restrict fb, int x0,
                                               int y0) {
  for (int py = 0; py < ORIC_QR_SIDE_PX; py++) {
    const int y = y0 + py;
    if (y < 0 || y >= ORIC_SCREEN_HEIGHT) {
      continue;
    }
    memset(line_buff, 0, sizeof(line_buff));
    // The module row under this scanline, or -1 while inside the quiet zone.
    const int mrow = (py / ORIC_QR_SCALE) - ORIC_QR_QUIET;
    for (int px = 0; px < ORIC_QR_SIDE_PX; px++) {
      const int x = x0 + px;
      if (x < 0 || x >= ORIC_SCREEN_WIDTH) {
        continue;
      }
      const int mcol = (px / ORIC_QR_SCALE) - ORIC_QR_QUIET;
      uint8_t dark = 0;
      if (mrow >= 0 && mrow < ORIC_QR_MODULES && mcol >= 0 &&
          mcol < ORIC_QR_MODULES) {
        const uint8_t bits = oric_qr_docs[mrow * ORIC_QR_STRIDE + (mcol >> 3)];
        dark = (bits >> (7 - (mcol & 7))) & 1u;
      }
      line_buff[x] = dark ? 0 : 7;  // black modules on white
    }
    _oric_pack_line(fb + (y * ATARI_ST_FRAMEBUFFER_LINE_SIZE_16WORDS),
                    line_buff);
  }
}

void oric_ovl_qr(oric_t* sys, int y0) {
  CHIPS_ASSERT(sys && sys->valid);
  _oric_draw_qr(sys->fb, (ORIC_SCREEN_WIDTH - ORIC_QR_SIDE_PX) / 2, y0);
}

#define ORIC_HINT_ROWS 8
static volatile bool oric_boot_hint_active;

static void __not_in_flash_func(_oric_draw_boot_hint)(uint16_t* restrict fb) {
  if (!oric_boot_hint_active) {
    return;
  }
  static const char kHint[] = "Press F1 for config menu";
  const int len = (int)(sizeof(kHint) - 1);
  const int start_x = (ORIC_SCREEN_WIDTH - (len * 8)) / 2;
  const int top = (ORIC_SCREEN_HEIGHT - ORIC_HINT_ROWS) / 2;
  for (int r = 0; r < ORIC_HINT_ROWS; r++) {
    memset(line_buff, 0, sizeof(line_buff));
    for (int i = 0; i < len; i++) {
      const uint8_t bits = oric_glyph_row(kHint[i], r);
      for (int b = 0; b < 8; b++) {
        const int x = start_x + i * 8 + b;
        if (x >= 0 && x < ORIC_SCREEN_WIDTH && (bits & (1u << b))) {
          line_buff[x] = 7;  // white
        }
      }
    }
    _oric_pack_line(fb + ((top + r) * ATARI_ST_FRAMEBUFFER_LINE_SIZE_16WORDS),
                    line_buff);
  }
}

#define ORIC_TAPE_BAR_ROWS 2
#define ORIC_TAPE_MSG_ROWS 8

static volatile bool oric_tape_bar_active;
static volatile uint32_t oric_tape_bar_pos;
static volatile uint32_t oric_tape_bar_size;
static volatile bool oric_tape_done_msg;

static void __not_in_flash_func(_oric_draw_tape_band)(oric_t* sys,
                                                      uint16_t* restrict fb) {
  if (oric_tape_done_msg) {
    static const char kDone[] = "TAPE FINISHED";
    const int len = (int)(sizeof(kDone) - 1);
    const int start_x = (ORIC_SCREEN_WIDTH - (len * 8)) / 2;
    for (int r = 0; r < ORIC_TAPE_MSG_ROWS; r++) {
      const int y = ORIC_SCREEN_HEIGHT - ORIC_TAPE_MSG_ROWS + r;
      memset(line_buff, 0, sizeof(line_buff));
      for (int i = 0; i < len; i++) {
        const uint8_t bits = oric_glyph_row(kDone[i], r);
        for (int b = 0; b < 8; b++) {
          const int x = start_x + i * 8 + b;
          if (x >= 0 && x < ORIC_SCREEN_WIDTH && (bits & (1u << b))) {
            line_buff[x] = 2;  // green
          }
        }
      }
      _oric_pack_line(fb + (y * ATARI_ST_FRAMEBUFFER_LINE_SIZE_16WORDS),
                      line_buff);
    }
    return;
  }

  if (!oric_tape_bar_active || oric_tape_bar_size == 0) {
    return;
  }
  uint32_t filled = (oric_tape_bar_pos * (uint32_t)ORIC_SCREEN_WIDTH) /
                    oric_tape_bar_size;
  if (filled > (uint32_t)ORIC_SCREEN_WIDTH) {
    filled = (uint32_t)ORIC_SCREEN_WIDTH;
  }
  for (int x = 0; x < ORIC_SCREEN_WIDTH; x++) {
    line_buff[x] = ((uint32_t)x < filled) ? 2u : 0u;  // green on black
  }
  for (int r = 0; r < ORIC_TAPE_BAR_ROWS; r++) {
    const int y = ORIC_SCREEN_HEIGHT - ORIC_TAPE_BAR_ROWS + r;
    _oric_pack_line(fb + (y * ATARI_ST_FRAMEBUFFER_LINE_SIZE_16WORDS),
                    line_buff);
  }
}

int __not_in_flash_func(oric_screen_update)(oric_t* sys) {
  bool dirty = sys->screen_dirty;
  if (!dirty) return 0;

  bool blink_state = (sys->blink_counter & 0x20) != 0;
  sys->blink_counter = (sys->blink_counter + 1) & 0x3F;

  // A mode the CPU latched since the last frame wins over the one the last
  // frame ended in. Clearing the flag first means a mode written while this
  // frame renders is picked up by the next one rather than being dropped.
  uint8_t pattr = sys->pattr;
  if (sys->pattr_pending_valid) {
    sys->pattr_pending_valid = false;
    __dmb();
    pattr = sys->pattr_pending;
  }
  uint8_t* restrict ram = sys->ram;

  uint16_t next_count = (uint16_t)(sys->fb_frame_counter + 1u);
  uint16_t* restrict fb = _oric_fb_for_count(next_count);
  sys->fb = fb;

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
      uint8_t* restrict dst8 = &line_buff[x * 6];
      dst8[0] = bits[0] ? c_fg : c_bg;
      dst8[1] = bits[1] ? c_fg : c_bg;
      dst8[2] = bits[2] ? c_fg : c_bg;
      dst8[3] = bits[3] ? c_fg : c_bg;
      dst8[4] = bits[4] ? c_fg : c_bg;
      dst8[5] = bits[5] ? c_fg : c_bg;
    }

    _oric_pack_line(dst_line, line_buff);
  }
  sys->pattr = pattr;

  _oric_draw_tape_band(sys, fb);
  _oric_draw_boot_hint(fb);

  sys->fb_frame_counter = next_count;
  uint8_t* fb_base = (uint8_t*)&__rom_in_ram_start__;
  uint16_t* fb_counter = (uint16_t*)(fb_base + ATARI_ST_FRAME_COUNTER_OFFSET);
  *fb_counter = next_count;

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
  kbd_register_key(&sys->kbd, 0x1B, 5, 1, 0);   // Esc (column 5, line 1)
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
  mem_snapshot_onload(&im.mem, sys);
  *sys = im;
  return true;
}

#endif  // CHIPS_IMPL
