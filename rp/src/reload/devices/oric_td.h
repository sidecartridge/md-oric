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
// SAFEGUARD: takes a filename, not an F-key index -- the menu chooses the
// file now (EPIC-05). The .tap is played directly, generated as it goes.
bool oric_td_insert_tape_sdcard(oric_td_t* sys, const char* filename);

// Remove the tape file from SD card
void oric_td_remove_tape_sdcard(oric_td_t* sys);

// SAFEGUARD: how far through the tape we are, for the on-screen loading bar.
// Returns false when nothing is playing.
bool oric_td_progress(const oric_td_t* sys, uint32_t* pos, uint32_t* size);

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
  uint32_t wave_size;
  uint8_t current_level;
  uint8_t shifter;
  uint8_t shift_count;
} oric_tap_stream_t;

// SAFEGUARD START: direct .tap playback.
//
// The "wave" file this device plays is not a real WAV -- it is a packed
// bitstream, one bit per tape signal level, with a 4-byte size header. The
// converter below builds it by pushing bytes at a file.
//
// Direct playback keeps that encoder byte-for-byte (it owns the bit timings
// that make real programs load) and only changes where the bytes go: into a
// small ring that the tape tick drains, refilled on demand. The whole stream
// is far too big for RAM -- roughly 4 bytes of bitstream per TAP byte -- so it
// has to be generated as it is consumed, which is why the TAP reader becomes
// the resumable state machine further down.
#define ORIC_TAP_RING_SIZE 256u
static uint8_t oric_tap_ring[ORIC_TAP_RING_SIZE];
static uint16_t oric_tap_ring_head;  // producer
static uint16_t oric_tap_ring_tail;  // consumer

static inline uint16_t oric_tap_ring_used(void) {
  return (uint16_t)((oric_tap_ring_head - oric_tap_ring_tail) &
                    (ORIC_TAP_RING_SIZE - 1u));
}
static inline uint16_t oric_tap_ring_free(void) {
  return (uint16_t)(ORIC_TAP_RING_SIZE - 1u - oric_tap_ring_used());
}
static inline bool oric_tap_ring_push(uint8_t value) {
  if (oric_tap_ring_free() == 0) {
    return false;
  }
  oric_tap_ring[oric_tap_ring_head] = value;
  oric_tap_ring_head =
      (uint16_t)((oric_tap_ring_head + 1u) & (ORIC_TAP_RING_SIZE - 1u));
  return true;
}
static inline bool oric_tap_ring_pop(uint8_t* value) {
  if (oric_tap_ring_used() == 0) {
    return false;
  }
  *value = oric_tap_ring[oric_tap_ring_tail];
  oric_tap_ring_tail =
      (uint16_t)((oric_tap_ring_tail + 1u) & (ORIC_TAP_RING_SIZE - 1u));
  return true;
}
// SAFEGUARD END

static bool oric_tap_write_byte(oric_tap_stream_t* st, uint8_t value) {
  st->wave_size++;
  return oric_tap_ring_push(value);
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

// SAFEGUARD START: resumable TAP -> bitstream generator.
//
// The same sequence the old file-based converter performed in straight-line
// code, split
// into phases so it can stop whenever the ring is full and pick up where it
// left off. Emitting one element per step keeps the ring requirement tiny: the
// largest single element is one encoded byte, about 26 half-periods.
enum {
  ORIC_TAPGEN_GAP = 0,
  ORIC_TAPGEN_FIND_SYNC,
  ORIC_TAPGEN_BIG_SYNC,
  ORIC_TAPGEN_HEADER,
  ORIC_TAPGEN_NAME,
  ORIC_TAPGEN_GAP2,
  ORIC_TAPGEN_DATA,
  ORIC_TAPGEN_GAP3,
  ORIC_TAPGEN_DONE
};

static oric_tap_input_t oric_tap_in;
static oric_tap_stream_t oric_tap_st;
static uint8_t oric_tap_phase;
static uint32_t oric_tap_count;      // elements left in the current phase
static uint32_t oric_tap_data_size;  // body length from the header
static uint8_t oric_tap_header[9];
static bool oric_tap_direct;  // true = generating from a .tap, no .wav involved

static void oric_tapgen_init(FIL* in, uint32_t size) {
  oric_tap_input_init(&oric_tap_in, in, size);
  oric_tap_st.wave_size = 0;
  oric_tap_st.current_level = 0;
  oric_tap_st.shifter = 0;
  oric_tap_st.shift_count = 0;
  oric_tap_ring_head = 0;
  oric_tap_ring_tail = 0;
  oric_tap_phase = ORIC_TAPGEN_GAP;
  oric_tap_count = 5;  // the leading gap the converter emits first
  oric_tap_data_size = 0;
}

// Emit one element. Returns false when the tape is finished.
static bool oric_tapgen_step(void) {
  uint8_t value = 0;
  switch (oric_tap_phase) {
    case ORIC_TAPGEN_GAP:
      (void)oric_tap_output_half_period(&oric_tap_st, 1);
      if (--oric_tap_count == 0) {
        oric_tap_phase = ORIC_TAPGEN_FIND_SYNC;
      }
      return true;

    case ORIC_TAPGEN_FIND_SYNC:
      // Consumes input without emitting, so it cannot overrun the ring.
      if (oric_tap_in.pos >= oric_tap_in.size ||
          !oric_tap_find_synchro(&oric_tap_in)) {
        oric_tap_phase = ORIC_TAPGEN_DONE;
        (void)oric_tap_flush_output(&oric_tap_st);
        return false;
      }
      oric_tap_phase = ORIC_TAPGEN_BIG_SYNC;
      oric_tap_count = 259;
      return true;

    case ORIC_TAPGEN_BIG_SYNC:
      if (oric_tap_count > 0) {
        (void)oric_tap_output_byte(&oric_tap_st, 0x16);
        oric_tap_count--;
      } else {
        (void)oric_tap_output_byte(&oric_tap_st, 0x24);
        oric_tap_phase = ORIC_TAPGEN_HEADER;
        oric_tap_count = 0;
      }
      return true;

    case ORIC_TAPGEN_HEADER:
      if (!oric_tap_read_byte(&oric_tap_in, &value)) {
        oric_tap_phase = ORIC_TAPGEN_DONE;
        (void)oric_tap_flush_output(&oric_tap_st);
        return false;
      }
      oric_tap_header[oric_tap_count] = value;
      (void)oric_tap_output_byte(&oric_tap_st, value);
      if (++oric_tap_count == sizeof(oric_tap_header)) {
        oric_tap_phase = ORIC_TAPGEN_NAME;
      }
      return true;

    case ORIC_TAPGEN_NAME:
      if (!oric_tap_read_byte(&oric_tap_in, &value)) {
        oric_tap_phase = ORIC_TAPGEN_DONE;
        (void)oric_tap_flush_output(&oric_tap_st);
        return false;
      }
      (void)oric_tap_output_byte(&oric_tap_st, value);
      if (value == 0) {
        oric_tap_phase = ORIC_TAPGEN_GAP2;
        oric_tap_count = 6;
      }
      return true;

    case ORIC_TAPGEN_GAP2: {
      (void)oric_tap_output_half_period(&oric_tap_st, 1);
      if (--oric_tap_count == 0) {
        const uint32_t start =
            (uint32_t)oric_tap_header[6] * 256u + oric_tap_header[7];
        const uint32_t end =
            (uint32_t)oric_tap_header[4] * 256u + oric_tap_header[5];
        if (end < start) {
          oric_tap_phase = ORIC_TAPGEN_DONE;
          (void)oric_tap_flush_output(&oric_tap_st);
          return false;
        }
        oric_tap_data_size = end - start + 1u;
        oric_tap_phase = ORIC_TAPGEN_DATA;
        oric_tap_count = 0;
      }
      return true;
    }

    case ORIC_TAPGEN_DATA:
      if (oric_tap_count >= oric_tap_data_size) {
        oric_tap_phase = ORIC_TAPGEN_GAP3;
        oric_tap_count = 2;
        return true;
      }
      if (!oric_tap_read_byte(&oric_tap_in, &value)) {
        oric_tap_phase = ORIC_TAPGEN_DONE;
        (void)oric_tap_flush_output(&oric_tap_st);
        return false;
      }
      (void)oric_tap_output_byte(&oric_tap_st, value);
      oric_tap_count++;
      return true;

    case ORIC_TAPGEN_GAP3:
      (void)oric_tap_output_half_period(&oric_tap_st, 1);
      if (--oric_tap_count == 0) {
        // Another file may follow on the same tape.
        oric_tap_phase = ORIC_TAPGEN_FIND_SYNC;
      }
      return true;

    default:
      return false;
  }
}

// Top up the ring. One encoded byte is at most ~26 half-periods = ~4 bytes,
// so 16 bytes of headroom is ample for a single step.
static void oric_tapgen_fill(void) {
  while (oric_tap_phase != ORIC_TAPGEN_DONE && oric_tap_ring_free() >= 16u) {
    if (!oric_tapgen_step()) {
      break;
    }
  }
}
// SAFEGUARD END

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
  sys->sd_have_byte = false;
  // SAFEGUARD: a reset rewinds the tape, it does not eject it. This used to
  // clear sd_file_open, so after any reset (HELP, RESET ORIC, a disk boot)
  // the drive silently played nothing while the menu still named the tape.
  if (sys->sd_file_open && oric_tap_direct) {
    (void)f_lseek(&sys->sd_file, 0);
    oric_tapgen_init(&sys->sd_file, (uint32_t)f_size(&sys->sd_file));
  }
}

void oric_td_tick_sdcard(oric_td_t* sys) {
  CHIPS_ASSERT(sys && sys->valid);
  static uint32_t last_logged_pos = (uint32_t)-1;
  if (!sys->sd_file_open) {
    return;
  }
  if (!oric_td_is_motor_on(sys)) {
    return;
  }

  if (!sys->sd_have_byte) {
    if (oric_tap_direct) {
      // Top up first: the generator only runs while the tape is moving, so it
      // costs nothing when the motor is off.
      oric_tapgen_fill();
      if (!oric_tap_ring_pop(&sys->sd_byte)) {
        return;  // generator finished and the ring has drained
      }
    } else {
      if (sys->size == 0 || sys->pos >= sys->size) {
        return;
      }
      UINT bytes_read = 0;
      FRESULT res = f_read(&sys->sd_file, &sys->sd_byte, 1, &bytes_read);
      if (res != FR_OK || bytes_read != 1) {
        sys->sd_have_byte = false;
        sys->size = 0;
        return;
      }
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
    if (!oric_tap_direct && (sys->pos % 1000u) == 0u &&
        sys->pos != last_logged_pos) {
      DPRINTF("Oric TD: read pos=%lu\n", (unsigned long)sys->pos);
      last_logged_pos = sys->pos;
    }
    sys->sd_have_byte = false;
  } else {
    sys->bit_pos--;
  }
}

bool oric_td_insert_tape_sdcard(oric_td_t* sys, const char* filename) {
  CHIPS_ASSERT(sys && sys->valid);
  if (!filename || filename[0] == '\0') {
    return false;
  }
  oric_td_remove_tape_sdcard(sys);
  sys->bit_pos = 7;

  SettingsConfigEntry* folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char* folder_name = folder ? folder->value : "/oric";

  // Derive the .tap path from the chosen name. The .wav path is built only so
  // a cache left by an older firmware can be deleted.
  char base[256];
  size_t blen = strlen(filename);
  if (blen >= sizeof(base)) {
    DPRINTF("Oric TD: tape name too long\n");
    return false;
  }
  memcpy(base, filename, blen + 1);
  char* dot = strrchr(base, '.');
  if (dot) {
    *dot = '\0';
  }

  char tap_path[256];
  // Only used to clear the stale cache an older firmware may have left here.
  char wav_path[256];
  int tap_len =
      snprintf(tap_path, sizeof(tap_path), "%s/%s.tap", folder_name, base);
  int wav_len =
      snprintf(wav_path, sizeof(wav_path), "%s/%s.wav", folder_name, base);
  if (tap_len <= 0 || (size_t)tap_len >= sizeof(tap_path) || wav_len <= 0 ||
      (size_t)wav_len >= sizeof(wav_path)) {
    DPRINTF("Oric TD: invalid tape path length\n");
    return false;
  }

  // SAFEGUARD: prefer the .tap and generate its bitstream as it plays. No
  // conversion pass, no delay on first load, and nothing written to the card.
  oric_tap_direct = false;
  FILINFO tap_info;
  if (f_stat(tap_path, &tap_info) == FR_OK &&
      f_open(&sys->sd_file, tap_path, FA_READ) == FR_OK) {
    oric_tapgen_init(&sys->sd_file, (uint32_t)tap_info.fsize);
    oric_tap_direct = true;
    sys->size = 0;
    sys->pos = 0;
    sys->sd_file_open = true;
    sys->sd_have_byte = false;
    DPRINTF("Oric TD: playing %s directly (%lu bytes)\n", tap_path,
            (unsigned long)tap_info.fsize);
    // Nothing generates .wav files any more, so a matching one here can only
    // be a cache written by an older firmware. Delete it: it is dead weight on
    // the card and would otherwise clutter the tape list forever.
    if (f_unlink(wav_path) == FR_OK) {
      DPRINTF("Oric TD: removed stale cache %s\n", wav_path);
    }
    return true;
  }

  DPRINTF("Oric TD: no .tap for %s\n", base);
  return false;
}

bool oric_td_progress(const oric_td_t* sys, uint32_t* pos, uint32_t* size) {
  if (!sys->valid || !sys->sd_file_open || !oric_tap_direct) {
    return false;
  }
  if (oric_tap_in.size == 0) {
    return false;
  }
  *pos = oric_tap_in.pos;
  *size = oric_tap_in.size;
  return true;
}

void oric_td_remove_tape_sdcard(oric_td_t* sys) {
  oric_tap_direct = false;
  oric_tap_ring_head = 0;
  oric_tap_ring_tail = 0;
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
