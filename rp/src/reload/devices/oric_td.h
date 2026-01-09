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

// Convert TAP image into WAVE image stored on SD card
bool oric_convert_tap_to_wave(const char* tap_path, const char* wave_path);

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
#include "hardware/structs/timer.h"
#ifndef CHIPS_ASSERT
#include <assert.h>
#define CHIPS_ASSERT(c) assert(c)
#endif

typedef struct {
  FIL* out;
  uint32_t wave_size;
  uint8_t current_level;
  uint8_t shifter;
  uint8_t shift_count;
} oric_tap_stream_t;

static bool oric_tap_write_byte(oric_tap_stream_t* st, uint8_t value) {
  UINT bytes_written = 0;
  FRESULT res = f_write(st->out, &value, 1, &bytes_written);
  if (res != FR_OK || bytes_written != 1) {
    return false;
  }
  st->wave_size++;
  return true;
}

static bool oric_tap_flush_output(oric_tap_stream_t* st) {
  for (int i = 0; i < 8 - st->shift_count; i++) {
    st->shifter = (uint8_t)((st->shifter << 1) | 1);
  }
  return oric_tap_write_byte(st, st->shifter);
}

static bool oric_tap_output_half_period(oric_tap_stream_t* st,
                                        uint8_t length) {
  for (int i = 0; i < length; i++) {
    st->shifter = (uint8_t)((st->shifter << 1) | st->current_level);
    st->shift_count++;
    if (st->shift_count == 8) {
      st->shift_count = 0;
      if (!oric_tap_write_byte(st, st->shifter)) {
        return false;
      }
    }
  }
  st->current_level ^= 1;
  return true;
}

static bool oric_tap_output_bit(oric_tap_stream_t* st, uint8_t bit) {
  if (!oric_tap_output_half_period(st, 1)) {
    return false;
  }
  return oric_tap_output_half_period(st, bit ? 1 : 2);
}

static bool oric_tap_output_byte(oric_tap_stream_t* st, uint8_t value) {
  if (!oric_tap_output_half_period(st, 1)) {
    return false;
  }
  if (!oric_tap_output_bit(st, 0)) {
    return false;
  }

  uint8_t parity = 1;

  for (int i = 0; i < 8; i++) {
    uint8_t bit = value & 1;
    parity = (uint8_t)(parity + bit);
    if (!oric_tap_output_bit(st, bit)) {
      return false;
    }
    value >>= 1;
  }

  if (!oric_tap_output_bit(st, parity & 1)) {
    return false;
  }

  if (!oric_tap_output_bit(st, 1)) {
    return false;
  }
  if (!oric_tap_output_bit(st, 1)) {
    return false;
  }
  return oric_tap_output_bit(st, 1);
}

#ifndef ORIC_TAP_INPUT_BUFFER_SIZE
#define ORIC_TAP_INPUT_BUFFER_SIZE 256
#endif

typedef struct {
  FIL* in;
  uint32_t size;
  uint32_t pos;
  uint16_t buf_pos;
  uint16_t buf_len;
  uint8_t buf[ORIC_TAP_INPUT_BUFFER_SIZE];
} oric_tap_input_t;

static void oric_tap_input_init(oric_tap_input_t* in_state,
                                FIL* in,
                                uint32_t size) {
  in_state->in = in;
  in_state->size = size;
  in_state->pos = 0;
  in_state->buf_pos = 0;
  in_state->buf_len = 0;
}

static bool oric_tap_read_byte(oric_tap_input_t* in_state, uint8_t* value) {
  if (in_state->pos >= in_state->size) {
    return false;
  }
  if (in_state->buf_pos >= in_state->buf_len) {
    UINT bytes_read = 0;
    FRESULT res =
        f_read(in_state->in, in_state->buf, sizeof(in_state->buf), &bytes_read);
    if (res != FR_OK || bytes_read == 0) {
      return false;
    }
    in_state->buf_pos = 0;
    in_state->buf_len = (uint16_t)bytes_read;
  }
  *value = in_state->buf[in_state->buf_pos++];
  in_state->pos++;
  return true;
}

static bool oric_tap_find_synchro(oric_tap_input_t* in_state) {
  int synchro_state = 0;
  uint8_t value = 0;
  while (in_state->pos < in_state->size) {
    if (!oric_tap_read_byte(in_state, &value)) {
      return false;
    }
    if (value == 0x16) {
      if (synchro_state < 3) {
        synchro_state++;
      }
    } else if (value == 0x24 && synchro_state == 3) {
      return true;
    } else {
      synchro_state = 0;
    }
  }
  return false;
}

static bool oric_tap_output_big_synchro(oric_tap_stream_t* st) {
  for (int i = 0; i < 259; i++) {
    if (!oric_tap_output_byte(st, 0x16)) {
      return false;
    }
  }
  return oric_tap_output_byte(st, 0x24);
}

static bool oric_tap_output_file(oric_tap_input_t* in_state,
                                 oric_tap_stream_t* st) {
  uint8_t header[9];
  uint32_t i = 0;

  while (in_state->pos < in_state->size && i < sizeof(header)) {
    if (!oric_tap_read_byte(in_state, &header[i])) {
      return false;
    }
    if (!oric_tap_output_byte(st, header[i])) {
      return false;
    }
    i++;
  }
  if (in_state->pos >= in_state->size) {
    return false;
  }

  uint8_t value = 0;
  while (in_state->pos < in_state->size) {
    if (!oric_tap_read_byte(in_state, &value)) {
      return false;
    }
    if (!oric_tap_output_byte(st, value)) {
      return false;
    }
    if (value == 0) {
      break;
    }
  }
  if (in_state->pos >= in_state->size && value != 0) {
    return false;
  }

  for (int j = 0; j < 6; j++) {
    if (!oric_tap_output_half_period(st, 1)) {
      return false;
    }
  }

  uint32_t start = (uint32_t)header[6] * 256u + header[7];
  uint32_t end = (uint32_t)header[4] * 256u + header[5];
  if (end < start) {
    return false;
  }
  uint32_t data_size = end - start + 1;
  i = 0;
  while (in_state->pos < in_state->size && i < data_size) {
    if (!oric_tap_read_byte(in_state, &value)) {
      return false;
    }
    if (!oric_tap_output_byte(st, value)) {
      return false;
    }
    i++;
  }
  if (in_state->pos == in_state->size && i < data_size) {
    return false;
  }

  for (int j = 0; j < 2; j++) {
    if (!oric_tap_output_half_period(st, 1)) {
      return false;
    }
  }

  return true;
}

bool oric_convert_tap_to_wave(const char* tap_path, const char* wave_path) {
  FIL in;
  FIL out;
  FRESULT res;
  UINT bytes_written = 0;
  uint64_t start_time = GET_CURRENT_TIME();

  if (!tap_path || !wave_path) {
    DPRINTF("Oric TD: convert_tap_to_wave invalid path\n");
    return false;
  }

  res = f_open(&in, tap_path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("Oric TD: convert_tap_to_wave open tap failed (%d): %s\n",
            (int)res,
            tap_path);
    return false;
  }

  res = f_open(&out, wave_path, FA_CREATE_ALWAYS | FA_WRITE);
  if (res != FR_OK) {
    DPRINTF("Oric TD: convert_tap_to_wave open wav failed (%d): %s\n",
            (int)res,
            wave_path);
    f_close(&in);
    return false;
  }

  uint8_t header[4] = {0, 0, 0, 0};
  res = f_write(&out, header, sizeof(header), &bytes_written);
  if (res != FR_OK || bytes_written != sizeof(header)) {
    DPRINTF("Oric TD: convert_tap_to_wave header write failed (%d)\n",
            (int)res);
    f_close(&out);
    f_close(&in);
    return false;
  }

  oric_tap_stream_t st = {
      .out = &out,
      .wave_size = 0,
      .current_level = 0,
      .shifter = 0,
      .shift_count = 0,
  };

  for (int i = 0; i < 5; i++) {
    if (!oric_tap_output_half_period(&st, 1)) {
      DPRINTF("Oric TD: convert_tap_to_wave gap write failed\n");
      f_close(&out);
      f_close(&in);
      return false;
    }
  }

  uint32_t size = f_size(&in);
  oric_tap_input_t in_state;
  oric_tap_input_init(&in_state, &in, size);
  uint32_t last_log_pos = 0;
  DPRINTF("Oric TD: convert_tap_to_wave start size=%lu\n",
          (unsigned long)size);
  while (in_state.pos < size) {
    if (oric_tap_find_synchro(&in_state)) {
      if (!oric_tap_output_big_synchro(&st)) {
        DPRINTF("Oric TD: convert_tap_to_wave big synchro failed\n");
        f_close(&out);
        f_close(&in);
        return false;
      }
      if (!oric_tap_output_file(&in_state, &st)) {
        DPRINTF("Oric TD: convert_tap_to_wave file output failed\n");
        f_close(&out);
        f_close(&in);
        return false;
      }
      if (in_state.pos - last_log_pos >= 4096) {
        DPRINTF("Oric TD: convert_tap_to_wave progress %lu/%lu\n",
                (unsigned long)in_state.pos,
                (unsigned long)size);
        last_log_pos = in_state.pos;
      }
    } else {
      break;
    }
  }

  if (!oric_tap_flush_output(&st)) {
    DPRINTF("Oric TD: convert_tap_to_wave flush failed\n");
    f_close(&out);
    f_close(&in);
    return false;
  }

  if (f_lseek(&out, 0) != FR_OK) {
    DPRINTF("Oric TD: convert_tap_to_wave seek failed\n");
    f_close(&out);
    f_close(&in);
    return false;
  }

  header[0] = (uint8_t)(st.wave_size & 0xFFu);
  header[1] = (uint8_t)((st.wave_size >> 8) & 0xFFu);
  header[2] = (uint8_t)((st.wave_size >> 16) & 0xFFu);
  header[3] = (uint8_t)((st.wave_size >> 24) & 0xFFu);

  res = f_write(&out, header, sizeof(header), &bytes_written);
  if (res != FR_OK || bytes_written != sizeof(header)) {
    DPRINTF("Oric TD: convert_tap_to_wave size write failed (%d)\n",
            (int)res);
    f_close(&out);
    f_close(&in);
    return false;
  }

  f_close(&out);
  f_close(&in);
  DPRINTF("Oric TD: convert_tap_to_wave done (%lu bytes) in %lu ms\n",
          (unsigned long)st.wave_size,
          (unsigned long)GET_CURRENT_TIME_INTERVAL_MS(start_time));
  return true;
}

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
  static uint32_t last_logged_pos = (uint32_t)-1;
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
    if ((sys->pos % 1000u) == 0u && sys->pos != last_logged_pos) {
      DPRINTF("Oric TD: read pos=%lu\n", (unsigned long)sys->pos);
      last_logged_pos = sys->pos;
    }
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
  char wav_path[256];
  char tap_path[256];
  int wav_len =
      snprintf(wav_path, sizeof(wav_path), "%s/f%d.wav", folder_name, index + 1);
  int tap_len =
      snprintf(tap_path, sizeof(tap_path), "%s/f%d.tap", folder_name, index + 1);
  if (wav_len <= 0 || (size_t)wav_len >= sizeof(wav_path) || tap_len <= 0 ||
      (size_t)tap_len >= sizeof(tap_path)) {
    DPRINTF("Oric TD: invalid tape path length\n");
    return false;
  }

  FRESULT res = f_open(&sys->sd_file, wav_path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("Oric TD: wav open failed (%d): %s\n", (int)res, wav_path);
    FILINFO info;
    res = f_stat(tap_path, &info);
    if (res != FR_OK) {
      DPRINTF("Oric TD: tap missing (%d): %s\n", (int)res, tap_path);
      return false;
    }
    DPRINTF("Oric TD: converting tap to wav: %s\n", tap_path);
    if (!oric_convert_tap_to_wave(tap_path, wav_path)) {
      DPRINTF("Oric TD: convert_tap_to_wave failed\n");
      return false;
    }
    res = f_open(&sys->sd_file, wav_path, FA_READ);
    if (res != FR_OK) {
      DPRINTF("Oric TD: wav open after convert failed (%d): %s\n",
              (int)res,
              wav_path);
      return false;
    }
  }

  uint8_t header[4];
  UINT bytes_read = 0;
  res = f_read(&sys->sd_file, header, sizeof(header), &bytes_read);
  if (res != FR_OK || bytes_read != sizeof(header)) {
    DPRINTF("Oric TD: wav header read failed (%d)\n", (int)res);
    f_close(&sys->sd_file);
    return false;
  }

  sys->size =
      header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
  sys->pos = 0;
  sys->sd_file_open = true;
  sys->sd_have_byte = false;
  DPRINTF("Oric TD: tape loaded size=%lu\n", (unsigned long)sys->size);
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
