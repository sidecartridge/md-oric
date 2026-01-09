#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "aconfig.h"
#include "ff.h"
#include "settings/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tape drive port bits
#define ORIC_TD_PORT_MOTOR (1 << 0)
#define ORIC_TD_PORT_READ (1 << 1)
#define ORIC_TD_PORT_WRITE (1 << 2)
#define ORIC_TD_PORT_PLAY (1 << 3)
#define ORIC_TD_PORT_RECORD (1 << 4)

// Oric tape drive state
typedef struct {
  uint8_t port;
  bool valid;
  uint32_t pos;
  uint32_t bit_pos;
  uint32_t size;
  uint8_t* wave_image;
  FIL sd_file;
  bool sd_file_open;
  uint8_t sd_byte;
  bool sd_have_byte;
} oric_td_t;

// Oric tape drive interface

// Initialize a new tape drive
void oric_td_init(oric_td_t* sys);

// Discard the tape drive
void oric_td_discard(oric_td_t* sys);

// Reset the tape drive
void oric_td_reset(oric_td_t* sys);

// Tick the tape drive reading from SD card
void oric_td_tick_sdcard(oric_td_t* sys);

// Insert a new tape file from SD card
bool oric_td_insert_tape_sdcard(oric_td_t* sys, int index);

// Remove the tape file from SD card
void oric_td_remove_tape_sdcard(oric_td_t* sys);

// Return true if the tape drive motor is on
bool oric_td_is_motor_on(oric_td_t* sys);

// Prepare a new tape drive snapshot for saving
void oric_td_snapshot_onsave(oric_td_t* snapshot);

// Fix up the tape drive snapshot after loading
void oric_td_snapshot_onload(oric_td_t* snapshot, oric_td_t* sys);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>  // memcpy, memset
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif

void oric_td_init(oric_td_t* sys) {
  CHIPS_ASSERT(sys && !sys->valid);
  memset(sys, 0, sizeof(oric_td_t));
  sys->valid = true;
  sys->bit_pos = 7;
}

void oric_td_discard(oric_td_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  sys->valid = false;
}

void oric_td_reset(oric_td_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  sys->port = 0;
  sys->size = 0;
  sys->pos = 0;
  sys->bit_pos = 7;
  sys->sd_file_open = false;
  sys->sd_have_byte = false;
}

void oric_td_tick_sdcard(oric_td_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  if (!sys->sd_file_open) {
    return;
  }
  if (!oric_td_is_motor_on(sys) || sys->size == 0 || sys->pos >= sys->size) {
    return;
  }

  if (!sys->sd_have_byte) {
    UINT bytes_read = 0;
    FRESULT res = f_read(&sys->sd_file, &sys->sd_byte, 1, &bytes_read);
    if (res != FR_OK || bytes_read != 1) {
      sys->sd_have_byte = false;
      sys->size = 0;
      return;
    }
    sys->sd_have_byte = true;
  }

  uint8_t b = sys->sd_byte;
  b >>= sys->bit_pos;
  if (b & 1) {
    sys->port |= ORIC_TD_PORT_READ;
  } else {
    sys->port &= ~ORIC_TD_PORT_READ;
  }
  if (sys->bit_pos == 0) {
    sys->bit_pos = 7;
    sys->pos++;
    sys->sd_have_byte = false;
  } else {
    sys->bit_pos--;
  }
}

bool oric_td_insert_tape_sdcard(oric_td_t* sys, int index) {
  CHIPS_ASSERT(sys && sys->valid);
  oric_td_remove_tape_sdcard(sys);
  sys->bit_pos = 7;

  SettingsConfigEntry* folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char* folder_name = folder ? folder->value : "/oric";
  char path[256];
  int path_len =
      snprintf(path, sizeof(path), "%s/tape%d.wav", folder_name, index);
  if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
    return false;
  }

  FRESULT res = f_open(&sys->sd_file, path, FA_READ);
  if (res != FR_OK) {
    return false;
  }

  uint8_t header[4];
  UINT bytes_read = 0;
  res = f_read(&sys->sd_file, header, sizeof(header), &bytes_read);
  if (res != FR_OK || bytes_read != sizeof(header)) {
    f_close(&sys->sd_file);
    return false;
  }

  sys->size =
      header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
  sys->pos = 0;
  sys->sd_file_open = true;
  sys->sd_have_byte = false;
  return true;
}

void oric_td_remove_tape_sdcard(oric_td_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  if (sys->sd_file_open) {
    f_close(&sys->sd_file);
    sys->sd_file_open = false;
  }
  sys->sd_have_byte = false;
  sys->size = 0;
  sys->pos = 0;
  sys->bit_pos = 7;
}

bool oric_td_is_motor_on(oric_td_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  return 0 != (sys->port & ORIC_TD_PORT_MOTOR);
}

void oric_td_snapshot_onsave(oric_td_t* snapshot) {
  CHIPS_ASSERT(snapshot);
  snapshot->port = 0;
}

void oric_td_snapshot_onload(oric_td_t* snapshot, oric_td_t* sys) {
  CHIPS_ASSERT(snapshot && sys);
  snapshot->port = sys->port;
}

#endif  // CHIPS_IMPL
