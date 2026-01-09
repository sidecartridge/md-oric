/**
 * File: kbdmap.c
 * Description: Atari ST GSX scan code to Oric key mapping.
 */

#include "kbdmap.h"

#include "pico/stdlib.h"

static uint16_t __not_in_flash() kbdmap_st_gsx_to_ascii[128][2] = {
  [0x01] = {'\x1b', '\x1b'},  // Esc
  [0x02] = {'1', '!'},
  [0x03] = {'2', '@'},
  [0x04] = {'3', '#'},
  [0x05] = {'4', '$'},
  [0x06] = {'5', '%'},
  [0x07] = {'6', '^'},
  [0x08] = {'7', '&'},
  [0x09] = {'8', '*'},
  [0x0A] = {'9', '('},
  [0x0B] = {'0', ')'},
  [0x0C] = {'-', 0x00A3},
  [0x0D] = {'=', '+'},
  [0x0E] = {'\b', '\b'},  // BS
  [0x0F] = {'\t', '\t'},  // TAB
  [0x10] = {'q', 'Q'},
  [0x11] = {'w', 'W'},
  [0x12] = {'e', 'E'},
  [0x13] = {'r', 'R'},
  [0x14] = {'t', 'T'},
  [0x15] = {'y', 'Y'},
  [0x16] = {'u', 'U'},
  [0x17] = {'i', 'I'},
  [0x18] = {'o', 'O'},
  [0x19] = {'p', 'P'},
  [0x1A] = {'[', '{'},
  [0x1B] = {']', '}'},
  [0x1C] = {'\r', '\r'},  // RET
  [0x1D] = {0, 0},        // CTRL
  [0x1E] = {'a', 'A'},
  [0x1F] = {'s', 'S'},
  [0x20] = {'d', 'D'},
  [0x21] = {'f', 'F'},
  [0x22] = {'g', 'G'},
  [0x23] = {'h', 'H'},
  [0x24] = {'j', 'J'},
  [0x25] = {'k', 'K'},
  [0x26] = {'l', 'L'},
  [0x27] = {';', ':'},
  [0x28] = {'\'', '\"'},
  [0x29] = {'`', '~'},
  [0x2A] = {0, 0},  // LEFT SHIFT
  [0x2B] = {'\\', '|'},
  [0x2C] = {'z', 'Z'},
  [0x2D] = {'x', 'X'},
  [0x2E] = {'c', 'C'},
  [0x2F] = {'v', 'V'},
  [0x30] = {'b', 'B'},
  [0x31] = {'n', 'N'},
  [0x32] = {'m', 'M'},
  [0x33] = {',', '<'},
  [0x34] = {'.', '>'},
  [0x35] = {'/', '?'},
  [0x36] = {0, 0},        // RIGHT SHIFT
  [0x38] = {0, 0},        // ALT
  [0x39] = {' ', ' '},    // SPACE
  [0x3A] = {0, 0},        // CAPS LOCK
  [0x3B] = {0, 0},        // F1
  [0x3C] = {0, 0},        // F2
  [0x3D] = {0, 0},        // F3
  [0x3E] = {0, 0},        // F4
  [0x3F] = {0, 0},        // F5
  [0x40] = {0, 0},        // F6
  [0x41] = {0, 0},        // F7
  [0x42] = {0, 0},        // F8
  [0x43] = {0, 0},        // F9
  [0x44] = {0, 0},        // F10
  [0x47] = {0, 0},        // HOME
  [0x48] = {0, 0},        // UP ARROW
  [0x4A] = {'-', '-'},    // KEYPAD -
  [0x4B] = {0, 0},        // LEFT ARROW
  [0x4D] = {0, 0},        // RIGHT ARROW
  [0x4E] = {'+', '+'},    // KEYPAD +
  [0x50] = {0, 0},        // DOWN ARROW
  [0x52] = {0, 0},        // INSERT
  [0x53] = {0x7F, 0x7F},  // DEL
  [0x60] = {0, 0},        // ISO KEY
  [0x61] = {0, 0},        // UNDO
  [0x62] = {0, 0},        // HELP
  [0x63] = {'(', '('},    // KEYPAD (
  [0x64] = {'/', '/'},    // KEYPAD /
  [0x65] = {'*', '*'},    // KEYPAD *
  [0x66] = {'*', '*'},    // KEYPAD *
  [0x67] = {'7', '7'},    // KEYPAD 7
  [0x68] = {'8', '8'},    // KEYPAD 8
  [0x69] = {'9', '9'},    // KEYPAD 9
  [0x6A] = {'4', '4'},    // KEYPAD 4
  [0x6B] = {'5', '5'},    // KEYPAD 5
  [0x6C] = {'6', '6'},    // KEYPAD 6
  [0x6D] = {'1', '1'},    // KEYPAD 1
  [0x6E] = {'2', '2'},    // KEYPAD 2
  [0x6F] = {'3', '3'},    // KEYPAD 3
  [0x70] = {'0', '0'},    // KEYPAD 0
  [0x71] = {'.', '.'},    // KEYPAD .
  [0x72] = {'\r', '\r'},  // KEYPAD ENTER
};

uint16_t __not_in_flash_func(kbdmap_StGsx2Ascii)(uint16_t scan_code,
                                                 bool use_upper) {
  if (scan_code > 0x7F) {
    return 0;
  }
  return kbdmap_st_gsx_to_ascii[scan_code][use_upper ? 1 : 0];
}

void kbdmap_initOric(void) {
  // Map F1..F10 to Oric keycodes 0x13A..0x143 in both lower/upper slots.
  for (uint16_t i = 0; i < 10; i++) {
    uint16_t keycode = (uint16_t)(0x13A + i);
    kbdmap_st_gsx_to_ascii[0x3B + i][0] = keycode;
    kbdmap_st_gsx_to_ascii[0x3B + i][1] = keycode;
  }

  // Map UNDO (0x61) and HELP (0x62).
  kbdmap_st_gsx_to_ascii[0x61][0] = 0x144;
  kbdmap_st_gsx_to_ascii[0x61][1] = 0x144;
  kbdmap_st_gsx_to_ascii[0x62][0] = 0x145;
  kbdmap_st_gsx_to_ascii[0x62][1] = 0x145;

  // Map arrow keys.
  kbdmap_st_gsx_to_ascii[0x4B][0] = 0x150;  // LEFT
  kbdmap_st_gsx_to_ascii[0x4B][1] = 0x150;
  kbdmap_st_gsx_to_ascii[0x4D][0] = 0x14F;  // RIGHT
  kbdmap_st_gsx_to_ascii[0x4D][1] = 0x14F;
  kbdmap_st_gsx_to_ascii[0x50][0] = 0x151;  // DOWN
  kbdmap_st_gsx_to_ascii[0x50][1] = 0x151;
  kbdmap_st_gsx_to_ascii[0x48][0] = 0x152;  // UP
  kbdmap_st_gsx_to_ascii[0x48][1] = 0x152;
}

bool __not_in_flash_func(kbdmap_isShift)(uint16_t scan_code) {
  return (scan_code == 0x2A) || (scan_code == 0x36);
}

bool __not_in_flash_func(kbdmap_isCtrl)(uint16_t scan_code) {
  return scan_code == 0x1D;
}
