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
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "font8x8.h"
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

// EPIC-03 STORY-01: cost of oric_screen_update, sampled on Core 1 and read on
// Core 0 when the stats key is pressed. Scalars, so volatile is enough to stop
// the compiler caching them across the loop (D-13); the barrier before reading
// keeps the four consistent with each other.
static volatile uint32_t oric_cvt_min_us = 0xFFFFFFFFu;
static volatile uint32_t oric_cvt_max_us = 0;
static volatile uint32_t oric_cvt_sum_us = 0;
static volatile uint32_t oric_cvt_count = 0;

// ---------------------------------------------------------------------------
// On-screen menu. Dispatcher shape follows md-gpu-demo's demo_menu.c: one key
// owned for entering/leaving, everything else forwarded to whatever is active.
// Here the owned key is F1 rather than ESC -- ESC is a real Oric key, whereas
// the Oric has no function keys, so F1 is free (D-15, D-16).
//
// State is written on Core 0 (the key handler) and read on Core 1 (the
// renderer), so it follows D-13: writer fills the payload, __dmb(), then
// raises the flag; reader tests the flag, __dmb(), then reads the payload.
// ---------------------------------------------------------------------------
enum {
  ORIC_UI_EMULATING = 0,
  ORIC_UI_MENU = 1,
  ORIC_UI_ROMLIST = 2,
  ORIC_UI_TAPELIST = 3,
  ORIC_UI_STATUS = 4
};


// File list. FatFs is built with long filenames (FF_MAX_LFN 255), but storing
// 255 bytes per entry would not fit the RAM budget, and the overlay is only 30
// columns wide anyway. Cap the stored name and count what is skipped rather
// than dropping it silently -- a user whose ROM does not appear deserves to
// know why.
#define ORIC_FILES_MAX 128
#define ORIC_NAME_MAX 48
#define ORIC_LIST_ROWS 18
#define ORIC_LIST_TOP 5

// Long enough to read, and long enough for the m68k to blit the final black
// frame at 50 Hz before the RP stops answering the cartridge bus.
#define ORIC_REBOOT_MSG_MS 800u
#define ORIC_BLACK_FRAME_MS 120u

static char oric_files[ORIC_FILES_MAX][ORIC_NAME_MAX]
    __attribute__((section(".oric_ram")));
static int oric_file_count;
static int oric_files_skipped;

// Name of the tape currently in the drive, for the menu and for eject.
static char oric_tape_name[ORIC_NAME_MAX];
static volatile uint16_t oric_list_sel;

static void oric_publish_msg(void);

static int load_oric_rom_from_sd(const char *romName);

// True once a ROM is actually loaded. Until then Core 0 must not tick the
// 6502 -- oric_rom is zeroed and the CPU would execute garbage.
static volatile bool oric_have_rom = false;

// Core 1 pause handshake. Saving the ROM choice writes flash from Core 0, and
// with PICO_FLASH_ASSUME_CORE0_SAFE=1 the SDK does not lock Core 1 out. The
// render path calls snprintf, which lives in flash, so Core 1 must be parked
// in RAM-resident code before the write.
static volatile bool oric_c1_pause_req = false;
static volatile bool oric_c1_paused = false;

static void oric_core1_pause(void) {
  oric_c1_pause_req = true;
  while (!oric_c1_paused) {
    tight_loop_contents();
  }
  __dmb();
}

static void oric_core1_resume(void) {
  __dmb();
  oric_c1_pause_req = false;
}

#define ORIC_MENU_ITEMS 5
static const char* const oric_menu_items[ORIC_MENU_ITEMS] = {
    "SELECT ROM", "SELECT TAPE", "EJECT TAPE", "STATUS", "RESUME"};

static volatile uint8_t oric_ui_state = ORIC_UI_EMULATING;
static volatile bool oric_ui_redraw = false;
static volatile uint8_t oric_menu_sel = 0;

#define ORIC_ATTR_NORMAL ORIC_OVL_ATTR(7, 0)  /* white on black */
#define ORIC_ATTR_DIM ORIC_OVL_ATTR(6, 0)     /* cyan on black */
#define ORIC_ATTR_HILITE ORIC_OVL_ATTR(0, 3)  /* black on yellow */

static const char* oric_folder_name(void) {
  SettingsConfigEntry* folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  return folder ? folder->value : "/oric";
}

// Case-insensitive suffix test.
static bool oric_name_has_ext(const char* name, const char* ext) {
  size_t n = strlen(name);
  size_t e = strlen(ext);
  if (n <= e) {
    return false;
  }
  const char* tail = name + (n - e);
  for (size_t i = 0; i < e; i++) {
    char a = tail[i];
    char b = ext[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) {
      return false;
    }
  }
  return true;
}

// Runs on Core 0 from the key handler, not from the per-frame path: it blocks
// on the SD card, so it must not sit inside the emulation or render loops.
static void oric_scan_files_ext(const char* ext) {

  DIR dir;
  FRESULT res = f_opendir(&dir, oric_folder_name());
  if (res != FR_OK) {
    DPRINTF("oric: opendir %s failed (%d)\n", oric_folder_name(), (int)res);
    return;
  }
  static FILINFO info;  // ~256 bytes with LFN; static to keep it off the stack
  while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
    // Skip directories, and anything the filesystem marks hidden or system.
    if (info.fattrib & (AM_DIR | AM_HID | AM_SYS)) {
      continue;
    }
    // Skip dot-files too. macOS writes an AppleDouble sidecar next to every
    // file it copies to a FAT card ("._basic.rom") and those are not always
    // flagged hidden. Left in, they clutter the list and -- worse -- make a
    // card holding one real ROM look like it holds two, which would suppress
    // the single-ROM auto-install at boot.
    if (info.fname[0] == '.') {
      continue;
    }
    if (!oric_name_has_ext(info.fname, ext)) {
      continue;
    }
    if (strlen(info.fname) >= ORIC_NAME_MAX) {
      oric_files_skipped++;
      continue;
    }
    if (oric_file_count >= ORIC_FILES_MAX) {
      oric_files_skipped++;
      continue;
    }
    (void)snprintf(oric_files[oric_file_count], ORIC_NAME_MAX, "%s",
                   info.fname);
    oric_file_count++;
  }
  f_closedir(&dir);
  DPRINTF("oric: %d files after '%s', %d skipped\n", oric_file_count, ext,
          oric_files_skipped);
}

static void oric_scan_files(const char* ext) {
  oric_file_count = 0;
  oric_files_skipped = 0;
  oric_scan_files_ext(ext);
}

// Tapes are .tap only. The drive plays them directly now, so nothing generates
// .wav files and there is no second extension to merge or de-duplicate.
static void oric_scan_tapes(void) {
  oric_file_count = 0;
  oric_files_skipped = 0;
  oric_scan_files_ext(".tap");
}

static void oric_menu_render(oric_t* sys) {
  oric_ovl_clear(ORIC_ATTR_NORMAL);
  oric_ovl_text(8, 2, "ORIC EMULATOR", ORIC_ATTR_NORMAL);
  oric_ovl_text(8, 3, "-------------", ORIC_ATTR_DIM);

  const uint8_t sel = oric_menu_sel;
  for (int i = 0; i < ORIC_MENU_ITEMS; i++) {
    const int row = 7 + i * 2;
    const uint8_t attr = (i == sel) ? ORIC_ATTR_HILITE : ORIC_ATTR_NORMAL;
    // Highlight the whole bar, not just the text, so the selection reads
    // clearly at 8x8 -- the cell attribute does the job a filled rect does
    // in md-gpu-demo.
    oric_ovl_fill(4, row, 22, attr);
    oric_ovl_text(6, row, oric_menu_items[i], attr);
  }

  SettingsConfigEntry* romEntry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM);
  const char* romName =
      (romEntry && romEntry->value[0] != '\0') ? romEntry->value : "(none)";
  char line[ORIC_OVL_COLS + 1];
  (void)snprintf(line, sizeof(line), "ROM: %s", romName);
  oric_ovl_text(2, 20, line, ORIC_ATTR_DIM);
  (void)snprintf(line, sizeof(line), "TAPE: %s",
                 oric_tape_name[0] ? oric_tape_name : "(none)");
  oric_ovl_text(2, 21, line, ORIC_ATTR_DIM);

  oric_ovl_text(2, 24, "UP/DN  RET=SELECT", ORIC_ATTR_DIM);
  oric_ovl_text(2, 25, "F1=CLOSE", ORIC_ATTR_DIM);
  oric_ovl_present(sys);
}

static void oric_list_render(oric_t* sys) {
  const bool roms = (oric_ui_state == ORIC_UI_ROMLIST);
  oric_ovl_clear(ORIC_ATTR_NORMAL);
  oric_ovl_text(2, 2, roms ? "SELECT ROM" : "SELECT TAPE", ORIC_ATTR_NORMAL);

  if (oric_file_count == 0) {
    if (!roms) {
      oric_ovl_text(1, 6, "No tape files found in", ORIC_ATTR_NORMAL);
      oric_ovl_text(1, 7, oric_folder_name(), ORIC_ATTR_DIM);
      oric_ovl_text(1, 9, "Copy .tap or .wav files", ORIC_ATTR_NORMAL);
      oric_ovl_text(1, 10, "there, then reopen this", ORIC_ATTR_NORMAL);
      oric_ovl_text(1, 11, "menu.", ORIC_ATTR_NORMAL);
      oric_ovl_text(1, 25, "F1=BACK", ORIC_ATTR_DIM);
      oric_ovl_present(sys);
      return;
    }
    // Reachable now that nothing is embedded (D-07 superseded), so it has to
    // say what to do rather than being an empty box.
    oric_ovl_text(1, 6, "No BASIC ROM file found in", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 7, oric_folder_name(), ORIC_ATTR_DIM);
    oric_ovl_text(1, 9, "Copy at least one .rom", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 10, "file there, then reopen", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 11, "this menu.", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 13, "Please read the", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 14, "microfirmware documentation", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 15, "to find one:", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 17, "docs.sidecartridge.com", ORIC_ATTR_DIM);
    oric_ovl_text(1, 25, "F1=BACK", ORIC_ATTR_DIM);
    oric_ovl_present(sys);
    return;
  }

  const int sel = (int)oric_list_sel;
  const int page = sel / ORIC_LIST_ROWS;
  const int first = page * ORIC_LIST_ROWS;
  const int pages = (oric_file_count + ORIC_LIST_ROWS - 1) / ORIC_LIST_ROWS;

  char hdr[ORIC_OVL_COLS + 1];
  (void)snprintf(hdr, sizeof(hdr), "%d FILES  PAGE %d/%d", oric_file_count,
                 page + 1, pages);
  oric_ovl_text(2, 3, hdr, ORIC_ATTR_DIM);

  for (int r = 0; r < ORIC_LIST_ROWS; r++) {
    const int idx = first + r;
    if (idx >= oric_file_count) {
      break;
    }
    const int row = ORIC_LIST_TOP + r;
    const uint8_t attr = (idx == sel) ? ORIC_ATTR_HILITE : ORIC_ATTR_NORMAL;
    oric_ovl_fill(1, row, ORIC_OVL_COLS - 2, attr);
    // oric_ovl_text clips at the right edge, so a long name simply truncates.
    oric_ovl_text(2, row, oric_files[idx], attr);
  }

  if (oric_files_skipped > 0) {
    char skip[ORIC_OVL_COLS + 1];
    (void)snprintf(skip, sizeof(skip), "%d SKIPPED (NAME TOO LONG)",
                   oric_files_skipped);
    oric_ovl_text(2, 23, skip, ORIC_ATTR_DIM);
  }
  oric_ovl_text(2, 25, "UP/DN  L/R=PAGE  F1=BACK", ORIC_ATTR_DIM);
  oric_ovl_present(sys);
}

static void oric_ui_repaint(void) {
  __dmb();
  oric_ui_redraw = true;
}

static void oric_romlist_open(void) {
  oric_scan_files(".rom");
  oric_list_sel = 0;
  oric_ui_repaint();
  oric_ui_state = ORIC_UI_ROMLIST;
}

// Copy the chosen ROM over rom.img, which is what the emulator loads at boot.
static bool oric_copy_to_default_rom(const char* name) {
  const char* folder = oric_folder_name();
  size_t flen = strlen(folder);
  const char* sep = (flen > 0 && folder[flen - 1] == '/') ? "" : "/";
  char src[256];
  char dst[256];
  if (snprintf(src, sizeof(src), "%s%s%s", folder, sep, name) <= 0 ||
      snprintf(dst, sizeof(dst), "%s%srom.img", folder, sep) <= 0) {
    return false;
  }

  FIL fs;
  FIL fd;
  if (f_open(&fs, src, FA_READ) != FR_OK) {
    DPRINTF("oric: cannot open %s\n", src);
    return false;
  }
  if (f_open(&fd, dst, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    DPRINTF("oric: cannot create %s\n", dst);
    f_close(&fs);
    return false;
  }

  static uint8_t copy_buf[1024];
  bool ok = true;
  for (;;) {
    UINT br = 0;
    if (f_read(&fs, copy_buf, sizeof(copy_buf), &br) != FR_OK) {
      ok = false;
      break;
    }
    if (br == 0) {
      break;
    }
    UINT bw = 0;
    if (f_write(&fd, copy_buf, br, &bw) != FR_OK || bw != br) {
      ok = false;
      break;
    }
  }
  f_close(&fd);
  f_close(&fs);
  DPRINTF("oric: copied %s -> %s (%s)\n", src, dst, ok ? "ok" : "FAILED");
  return ok;
}

// Copy the choice over rom.img and reboot. Rebooting rather than swapping the
// ROM under a running 6502 means the emulator reinitialises from a clean state,
// with no half-reset CPU, tape drive or VIA to reason about.
// Reject a file that cannot be an Oric ROM before it overwrites rom.img.
// Without this the copy succeeds, the reboot fails to load, and the user lands
// back on the picker with nothing explaining why.
static bool oric_rom_size_ok(const char* name, uint32_t* out_size) {
  const char* folder = oric_folder_name();
  size_t flen = strlen(folder);
  const char* sep = (flen > 0 && folder[flen - 1] == '/') ? "" : "/";
  char path[256];
  if (snprintf(path, sizeof(path), "%s%s%s", folder, sep, name) <= 0) {
    return false;
  }
  FILINFO info;
  if (f_stat(path, &info) != FR_OK) {
    return false;
  }
  *out_size = (uint32_t)info.fsize;
  return info.fsize >= (FSIZE_t)ORIC_ROM_SIZE;
}

static void oric_select_rom(oric_t* sys, const char* name) {
  // Park Core 1 for the whole sequence. It must not publish frames over the
  // screens painted below, and settings_save writes flash while
  // PICO_FLASH_ASSUME_CORE0_SAFE=1 leaves Core 1 unlocked.
  oric_core1_pause();

  uint32_t size = 0;
  if (!oric_rom_size_ok(name, &size)) {
    char msg[ORIC_OVL_COLS + 1];
    (void)snprintf(msg, sizeof(msg), "%s is %lu bytes", name,
                   (unsigned long)size);
    oric_ovl_clear(ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 8, "Not a valid Oric ROM.", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 10, msg, ORIC_ATTR_DIM);
    oric_ovl_text(1, 12, "A BASIC ROM is 16384 bytes", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 13, "(16 KB).", ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 25, "F1=BACK", ORIC_ATTR_DIM);
    oric_ovl_present(sys);
    oric_core1_resume();
    return;  // rom.img is left untouched
  }

  oric_ovl_clear(ORIC_ATTR_NORMAL);
  oric_ovl_text(1, 10, "Installing ROM...", ORIC_ATTR_NORMAL);
  oric_ovl_present(sys);

  if (!oric_copy_to_default_rom(name)) {
    char msg[ORIC_OVL_COLS + 1];
    (void)snprintf(msg, sizeof(msg), "Cannot copy %s", name);
    oric_ovl_clear(ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 8, msg, ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 10, "Check the card is writable.", ORIC_ATTR_DIM);
    oric_ovl_text(1, 25, "F1=BACK", ORIC_ATTR_DIM);
    oric_ovl_present(sys);
    oric_core1_resume();
    return;  // stay on the list rather than dead-ending
  }

  // Remember which file it came from, so the menu can show it after the
  // reboot. Core 1 is already parked; it is never resumed from here.
  (void)settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM, name);
  (void)settings_save(aconfig_getContext(), true);

  oric_ovl_clear(ORIC_ATTR_NORMAL);
  oric_ovl_text(1, 10, "Rebooting...", ORIC_ATTR_NORMAL);
  oric_ovl_present(sys);
  sleep_ms(ORIC_REBOOT_MSG_MS);

  // Leave the screen black before resetting. Once the RP reboots the frame
  // counter stops changing, so the m68k never blits again and the ST holds
  // whatever was last on screen -- indefinitely. If the chosen ROM turns out
  // to be broken, a frozen menu would look live and mislead; black does not.
  oric_ovl_clear(ORIC_OVL_ATTR(0, 0));
  oric_ovl_present(sys);
  // The m68k only picks this up on its next VBL, so give it time to actually
  // blit the black frame before the bus goes away.
  sleep_ms(ORIC_BLACK_FRAME_MS);

  watchdog_reboot(0, 0, RESET_WATCHDOG_TIMEOUT);
  while (1) {
    tight_loop_contents();
  }
}

static void oric_set_msg(const char* text) {
  (void)snprintf(oric_msg_buf, sizeof(oric_msg_buf), "%s", text);
  oric_publish_msg();
}

static void oric_tapelist_open(void) {
  oric_scan_tapes();
  oric_list_sel = 0;
  oric_ui_repaint();
  oric_ui_state = ORIC_UI_TAPELIST;
}

static void oric_insert_tape(oric_t* sys, const char* name) {
  char msg[ORIC_OVL_COLS + 1];
  if (!sys->td.valid) {
    return;
  }
  if (!oric_td_insert_tape_sdcard(&sys->td, name)) {
    oric_ovl_clear(ORIC_ATTR_NORMAL);
    (void)snprintf(msg, sizeof(msg), "Cannot load %s", name);
    oric_ovl_text(1, 9, msg, ORIC_ATTR_NORMAL);
    oric_ovl_text(1, 25, "F1=BACK", ORIC_ATTR_DIM);
    oric_ovl_present(sys);
    return;  // stay on the list
  }
  (void)snprintf(oric_tape_name, sizeof(oric_tape_name), "%s", name);
  // Back to the emulator with the usual transient confirmation, so the user
  // can go straight to CLOAD"".
  (void)snprintf(msg, sizeof(msg), "Loading %s", name);
  oric_ui_state = ORIC_UI_EMULATING;
  sys->screen_dirty = true;
  oric_set_msg(msg);
}

static void oric_eject_tape(oric_t* sys) {
  if (!sys->td.valid || oric_tape_name[0] == '\0') {
    oric_set_msg("No tape inserted");
  } else {
    oric_td_remove_tape_sdcard(&sys->td);
    oric_tape_name[0] = '\0';
    oric_set_msg("Tape ejected");
  }
  oric_ui_state = ORIC_UI_EMULATING;
  sys->screen_dirty = true;
}

static bool oric_romlist_key(oric_t* sys, int code) {
  (void)sys;
  if (oric_file_count == 0) {
    return true;  // only F1 gets out, and that is handled before we are called
  }
  const int n = oric_file_count;
  int sel = (int)oric_list_sel;
  switch (code) {
    // Clamp rather than wrap: on a long list, rolling from the last entry back
    // to the first loses your place and reads as a glitch.
    case 0x152:  // UP
      sel = (sel > 0) ? sel - 1 : 0;
      break;
    case 0x151:  // DOWN
      sel = (sel < n - 1) ? sel + 1 : n - 1;
      break;
    case 0x150:  // LEFT: previous page
      sel = (sel >= ORIC_LIST_ROWS) ? sel - ORIC_LIST_ROWS : 0;
      break;
    case 0x14F:  // RIGHT: next page
      sel = (sel + ORIC_LIST_ROWS < n) ? sel + ORIC_LIST_ROWS : n - 1;
      break;
    case '\r':
      if (oric_ui_state == ORIC_UI_TAPELIST) {
        oric_insert_tape(sys, oric_files[sel]);
      } else {
        oric_select_rom(sys, oric_files[sel]);
      }
      return true;
    default:
      return true;
  }
  oric_list_sel = (uint16_t)sel;
  oric_ui_repaint();
  return true;
}

static void oric_status_render(oric_t* sys) {
  char line[ORIC_OVL_COLS + 1];

  oric_ovl_clear(ORIC_ATTR_NORMAL);
  oric_ovl_text(2, 2, "STATUS", ORIC_ATTR_NORMAL);

#ifdef RELEASE_VERSION
  (void)snprintf(line, sizeof(line), "Version %s", RELEASE_VERSION);
  oric_ovl_text(2, 4, line, ORIC_ATTR_DIM);
#endif

  SettingsConfigEntry* romEntry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM);
  const char* romName =
      (romEntry && romEntry->value[0] != '\0') ? romEntry->value : "(none)";
  (void)snprintf(line, sizeof(line), "ROM:  %s", romName);
  oric_ovl_text(2, 6, line, ORIC_ATTR_NORMAL);
  (void)snprintf(line, sizeof(line), "TAPE: %s",
                 oric_tape_name[0] ? oric_tape_name : "(none)");
  oric_ovl_text(2, 7, line, ORIC_ATTR_NORMAL);

  // Conversion timing. Nothing is converted while this screen is up -- Core 1
  // is rendering the menu, not the Oric screen -- so the numbers are a stable
  // snapshot of the run up to the moment the menu was opened.
  __dmb();
  uint32_t count = oric_cvt_count;
  uint32_t sum = oric_cvt_sum_us;
  uint32_t lo = oric_cvt_min_us;
  uint32_t hi = oric_cvt_max_us;

  oric_ovl_text(2, 10, "Screen conversion (us)", ORIC_ATTR_NORMAL);
  if (count == 0) {
    oric_ovl_text(3, 12, "no frames measured yet", ORIC_ATTR_DIM);
  } else {
    const uint32_t mean = sum / count;
    (void)snprintf(line, sizeof(line), "min  %lu", (unsigned long)lo);
    oric_ovl_text(3, 12, line, ORIC_ATTR_DIM);
    (void)snprintf(line, sizeof(line), "mean %lu", (unsigned long)mean);
    oric_ovl_text(3, 13, line, ORIC_ATTR_DIM);
    (void)snprintf(line, sizeof(line), "max  %lu", (unsigned long)hi);
    oric_ovl_text(3, 14, line, ORIC_ATTR_DIM);
    // 19968 us is Core 1's per-frame budget; the percentage is the number
    // that actually matters when judging headroom.
    (void)snprintf(line, sizeof(line), "%lu%% of the 19968 budget",
                   (unsigned long)((mean * 100u) / 19968u));
    oric_ovl_text(3, 16, line, ORIC_ATTR_DIM);
  }

  oric_ovl_text(1, 24, "HOME shows timing without", ORIC_ATTR_DIM);
  oric_ovl_text(1, 25, "opening this menu. F1=BACK", ORIC_ATTR_DIM);
  oric_ovl_present(sys);
}

static void oric_status_open(void) {
  oric_ui_repaint();
  oric_ui_state = ORIC_UI_STATUS;
}

static void oric_menu_open(void) {
  oric_menu_sel = 0;
  oric_ui_repaint();
  oric_ui_state = ORIC_UI_MENU;
}

static void oric_menu_close(oric_t* sys) {
  oric_ui_state = ORIC_UI_EMULATING;
  // Force a full repaint of the Oric screen: the menu overwrote the
  // framebuffer, and oric_screen_update only renders when something is dirty.
  sys->screen_dirty = true;
}

// Returns true if the key was consumed by the menu.
static bool oric_menu_key(oric_t* sys, int code) {
  switch (code) {
    case 0x152:  // UP
      oric_menu_sel =
          (uint8_t)((oric_menu_sel + ORIC_MENU_ITEMS - 1) % ORIC_MENU_ITEMS);
      oric_ui_repaint();
      return true;
    case 0x151:  // DOWN
      oric_menu_sel = (uint8_t)((oric_menu_sel + 1) % ORIC_MENU_ITEMS);
      oric_ui_repaint();
      return true;
    case '\r':  // RETURN
      switch (oric_menu_sel) {
        case 0:
          oric_romlist_open();
          break;
        case 1:
          oric_tapelist_open();
          break;
        case 2:
          oric_eject_tape(sys);
          break;
        case 3:
          oric_status_open();
          break;
        case ORIC_MENU_ITEMS - 1:
          oric_menu_close(sys);
          break;
        default:
          break;
      }
      return true;
    default:
      return true;  // menu owns the keyboard while it is open
  }
}


#ifndef ORIC_MSG_DISPLAY_SECONDS
#define ORIC_MSG_DISPLAY_SECONDS 3u
#endif

// Free-running fallback period when no blit-finished signal is arriving. Longer
// than a PAL frame so it never races the ST when the handshake is healthy.
#define ORIC_FRAME_FALLBACK_US 25000u

// Payload then flag, always: Core 1 keys off the deadline, so the text must be
// complete and visible before the deadline is raised (D-13).
static void oric_publish_msg(void) {
  __dmb();
  oric_msg_until_us = time_us_32() + (ORIC_MSG_DISPLAY_SECONDS * 1000u * 1000u);
}

// Show min / mean / max microseconds per conversion, then start a fresh
// sample window so the next program measured is not polluted by this one.
static void oric_show_cvt_stats(void) {
  __dmb();
  uint32_t count = oric_cvt_count;
  uint32_t sum = oric_cvt_sum_us;
  uint32_t lo = oric_cvt_min_us;
  uint32_t hi = oric_cvt_max_us;
  if (count == 0) {
    (void)snprintf(oric_msg_buf, sizeof(oric_msg_buf), "NO DATA");
  } else {
    (void)snprintf(oric_msg_buf, sizeof(oric_msg_buf), "%lu/%lu/%lu us",
                   (unsigned long)lo, (unsigned long)(sum / count),
                   (unsigned long)hi);
  }
  oric_cvt_min_us = 0xFFFFFFFFu;
  oric_cvt_max_us = 0;
  oric_cvt_sum_us = 0;
  oric_cvt_count = 0;
  oric_publish_msg();
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

uint8_t __attribute__((section(".oric_ram")))
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

static int load_oric_rom_from_sd(const char *romName) {
  if (!romName || romName[0] == '\0') {
    return ORIC_ROM_LOAD_ERR_PATH;
  }
  const char *folderName = oric_folder_name();
  char path[256];
  size_t name_len = strlen(folderName);
  const char *sep =
      (name_len > 0 && folderName[name_len - 1] == '/') ? "" : "/";
  int path_len =
      snprintf(path, sizeof(path), "%s%s%s", folderName, sep, romName);
  if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
    DPRINTF("oric: rom path too long\n");
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

  // F1 owns the UI: it steps back one level, and opens the menu from the
  // emulator.
  if (code == 0x13A) {
    switch (oric_ui_state) {
      case ORIC_UI_ROMLIST:
      case ORIC_UI_TAPELIST:
      case ORIC_UI_STATUS:
        oric_ui_state = ORIC_UI_MENU;
        oric_ui_repaint();
        break;
      case ORIC_UI_MENU:
        oric_menu_close(sys);
        break;
      default:
        oric_menu_open();
        break;
    }
    return;
  }
  // Any UI screen consumes everything; nothing reaches the Oric.
  if (oric_ui_state == ORIC_UI_MENU) {
    (void)oric_menu_key(sys, code);
    return;
  }
  if (oric_ui_state == ORIC_UI_ROMLIST || oric_ui_state == ORIC_UI_TAPELIST) {
    (void)oric_romlist_key(sys, code);
    return;
  }
  if (oric_ui_state == ORIC_UI_STATUS) {
    return;  // read-only screen; F1 above is the way out
  }

  switch (code) {

    case 0x148:  // ST HOME: show conversion timing, then reset the window
      oric_show_cvt_stats();
      break;

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
  // Swallow releases while the menu is open, and the F1 release always --
  // otherwise the Oric sees a release for a press it never got.
  if (oric_ui_state != ORIC_UI_EMULATING || code == 0x13A) {
    return;
  }
  kbd_key_up(&state.oric.kbd, code);
}

void gamepad_state_update(uint8_t index, uint8_t hat_state,
                          uint32_t button_state) {}

void __not_in_flash_func(core1_main()) {
  uint32_t next_update_us = time_us_32();
  uint32_t last_blit_done = emul_blitDoneCount;
  while (1) {
    if (oric_c1_pause_req) {
      oric_c1_paused = true;
      while (oric_c1_pause_req) {
        tight_loop_contents();
      }
      oric_c1_paused = false;
      continue;
    }
    uint32_t now_us = time_us_32();
    // Pace on the m68k's "blit finished" signal rather than free-running, so a
    // frame is never started while the ST is still reading the buffer it would
    // land in, and Core 1 stops drifting against the ST's VBL (D-12, D-10).
    // The timeout is the safety net: the m68k stops signalling whenever the ST
    // is reset or running anything but the blit loop, and a Core 1 that waited
    // forever would be a dead display with no obvious cause.
    uint32_t blit_done = emul_blitDoneCount;
    bool blitted = (blit_done != last_blit_done);
    if (blitted) {
      __dmb();
      last_blit_done = blit_done;
    }
    if (blitted || (int32_t)(now_us - next_update_us) >= 0) {
      if (oric_ui_state != ORIC_UI_EMULATING) {
        if (oric_ui_redraw) {
          oric_ui_redraw = false;
          __dmb();
          if (oric_ui_state == ORIC_UI_ROMLIST ||
              oric_ui_state == ORIC_UI_TAPELIST) {
            oric_list_render(&state.oric);
          } else if (oric_ui_state == ORIC_UI_STATUS) {
            oric_status_render(&state.oric);
          } else {
            oric_menu_render(&state.oric);
          }
        }
        next_update_us = now_us + 19968;
        continue;
      }
      uint32_t until_us = oric_msg_until_us;
      if (until_us != 0 && (int32_t)(until_us - now_us) > 0) {
        // oric_msg_buf is written by Core 0 and is not volatile, so without a
        // barrier here the compiler may keep oric_msg_buf[0] in a register
        // across iterations: it then sees the empty buffer from boot forever,
        // the inlined `*msg == '\0'` check skips oric_show_msg, and nothing
        // renders for the whole message window (display freezes, no overlay).
        __dmb();
        oric_show_msg(&state.oric, oric_msg_buf);
      } else {
        if (until_us != 0) {
          oric_msg_until_us = 0;
          // The overlay was covering the Oric screen; force a repaint so it
          // comes back even if the Oric has written nothing since (a tape
          // load writes nothing to screen RAM for seconds).
          state.oric.screen_dirty = true;
        }
        // Tape loading band. The Oric writes nothing to screen RAM during a
        // load, so nothing would repaint on its own -- force a redraw, but
        // only when the bar would actually move, so a load does not cost a
        // full conversion every frame.
        static uint32_t bar_last_filled = 0xFFFFFFFFu;
        static uint32_t tape_done_until_us;
        uint32_t tpos = 0;
        uint32_t tsize = 0;
        if (oric_td_progress(&state.oric.td, &tpos, &tsize) &&
            oric_td_is_motor_on(&state.oric.td) && tpos < tsize) {
          oric_tape_bar_pos = tpos;
          oric_tape_bar_size = tsize;
          oric_tape_bar_active = true;
          oric_tape_done_msg = false;
          const uint32_t filled =
              (tpos * (uint32_t)ORIC_SCREEN_WIDTH) / tsize;
          if (filled != bar_last_filled) {
            bar_last_filled = filled;
            state.oric.screen_dirty = true;
          }
        } else if (oric_tape_bar_active) {
          oric_tape_bar_active = false;
          bar_last_filled = 0xFFFFFFFFu;
          // Only call it finished if the tape actually ran out. A motor stop
          // part-way through is a pause or an eject, not a completed load, and
          // saying "finished" there would be a lie.
          if (tsize != 0 && tpos >= tsize) {
            oric_tape_done_msg = true;
            tape_done_until_us = now_us + 2000000u;
          }
          state.oric.screen_dirty = true;
        } else if (oric_tape_done_msg &&
                   (int32_t)(tape_done_until_us - now_us) <= 0) {
          oric_tape_done_msg = false;
          state.oric.screen_dirty = true;  // repaint to clear the band
        }

        uint32_t cvt_t0 = time_us_32();
        if (oric_screen_update(&state.oric)) {
          uint32_t dt = time_us_32() - cvt_t0;
          if (dt < oric_cvt_min_us) oric_cvt_min_us = dt;
          if (dt > oric_cvt_max_us) oric_cvt_max_us = dt;
          oric_cvt_sum_us += dt;
          oric_cvt_count++;
        }
      }
      next_update_us = now_us + ORIC_FRAME_FALLBACK_US;
    }
  }
  __builtin_unreachable();
}

int __not_in_flash_func(oric_main)() {
  // Erase the ROM area in RAM
  memset((void *)&__oric_rom_in_ram_start__, 0, 32 * 1024 * sizeof(uint8_t));
  int rom_load_result = load_oric_rom_from_sd("rom.img");

  // SAFEGUARD START: Init translation table for Oric
  kbdmap_initOric();
  build_oric_pat_lut();

  // SAFEGUARD END

  app_init();

  uint8_t *fb_base = (uint8_t *)&__rom_in_ram_start__;
  oric_via_queue = (uint16_t *)(fb_base + ATARI_ST_VIA_QUEUE_OFFSET);
  oric_via_queue_head = 0;
  memset(oric_via_queue, 0xFF, ATARI_ST_VIA_QUEUE_SIZE_BYTES);

  // The firmware copy does not reach 0x0FFC, so the counter would otherwise
  // start as uninitialised SRAM. Both sides start at 0 and agree.
  *(uint16_t *)(fb_base + ATARI_ST_FRAME_COUNTER_OFFSET) = 0;

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

  if (rom_load_result == ORIC_ROM_LOAD_OK) {
    oric_have_rom = true;
  } else {
    // No ROM installed yet, or the installed one is unreadable.
    DPRINTF("oric: rom.img load error %d\n", rom_load_result);
    oric_scan_files(".rom");
    if (oric_file_count == 1) {
      // Exactly one candidate: install it without making the user choose from
      // a list of one. Announce it first so it is visible rather than silent
      // -- oric_select_rom copies over rom.img and reboots, so the message is
      // the only trace the user would otherwise get.
      char msg[ORIC_OVL_COLS + 1];
      (void)snprintf(msg, sizeof(msg), "Installing %s", oric_files[0]);
      oric_ovl_clear(ORIC_ATTR_NORMAL);
      oric_ovl_text(1, 10, msg, ORIC_ATTR_NORMAL);
      oric_ovl_text(1, 12, "Only ROM found on the card.", ORIC_ATTR_DIM);
      oric_ovl_present(&state.oric);
      sleep_ms(1500);
      oric_select_rom(&state.oric, oric_files[0]);
      // oric_select_rom reboots on success; reaching here means the copy
      // failed and it has already painted the reason, so fall through to the
      // list rather than rebooting into the same failure.
    }
    // Zero ROMs, several ROMs, or a failed auto-install: let the user pick.
    oric_ui_state = ORIC_UI_ROMLIST;
    oric_list_sel = 0;
    oric_ui_repaint();
  }

  DPRINTF("Core 1 start\n");
  multicore_launch_core1(core1_main);

  uint32_t num_ticks = 19968;
  while (1) {
    uint32_t start_time_in_micros = time_us_32();

    if (oric_have_rom) {
      for (uint32_t ticks = 0; ticks < num_ticks; ticks++) {
        oric_tick(&state.oric);
      }
    }

    static bool shift_pressed = false;
    static bool ctrl_pressed = false;
    uint16_t addr_value = 0;
    if (emul_addrlog_pop(&addr_value)) {
      if ((addr_value & 0xFFF) == CMD_KEYPRESS ||
          (addr_value & 0xFFF) == CMD_KEYRELEASE) {
        uint16_t key_value = 0;
        if (emul_addrlog_pop(&key_value)) {
          bool is_press = ((addr_value & 0xFFF) == CMD_KEYPRESS);
          uint16_t scan_code = key_value & 0x7F;
          if (kbdmap_isShift(scan_code)) {
            if (is_press) {
              kbd_raw_key_down(ORIC_KEY_SHIFT);
            } else {
              kbd_raw_key_up(ORIC_KEY_SHIFT);
            }
            shift_pressed = is_press;
            continue;
          }
          if (kbdmap_isCtrl(scan_code)) {
            if (is_press) {
              kbd_raw_key_down(ORIC_KEY_CTRL);
            } else {
              kbd_raw_key_up(ORIC_KEY_CTRL);
            }
            ctrl_pressed = is_press;
            continue;
          }
          DPRINTF("scan_code: $%02x, %s, shift: %c\n", scan_code,
                  is_press ? "DOWN" : "UP", shift_pressed ? 'Y' : 'N');
          uint16_t ascii_value =
              kbdmap_StGsx2Ascii(scan_code, shift_pressed, ctrl_pressed);
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
