/**
 * File: kbdmap.h
 * Description: Atari ST GSX scan code to Oric key mapping.
 */

#ifndef KBDMAP_H
#define KBDMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

uint16_t __not_in_flash_func(kbdmap_StGsx2Ascii)(uint16_t scan_code,
                                                 bool use_upper,
                                                 bool use_ctrl);
void kbdmap_initOric(void);
bool __not_in_flash_func(kbdmap_isShift)(uint16_t scan_code);
bool __not_in_flash_func(kbdmap_isCtrl)(uint16_t scan_code);

#endif  // KBDMAP_H
