/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025
 * Copyright: 2025 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"

#include "reload/systems/oric/src/oric.h"

// Include the target firmware binary.
#include "target_firmware.h"

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// Ring buffer for DMA LSB lookup values.
#define EMUL_ADDRLOG_CAPACITY 16
volatile uint16_t __not_in_flash() addrlog_buf[EMUL_ADDRLOG_CAPACITY];
volatile size_t addrlog_head = 0;
volatile size_t addrlog_tail = 0;
volatile size_t addrlog_count = 0;

void __not_in_flash_func(emul_addrlog_clear)(void) {
  addrlog_head = 0;
  addrlog_tail = 0;
  addrlog_count = 0;
}

bool __not_in_flash_func(emul_addrlog_pop)(uint16_t *value) {
  if (addrlog_count == 0) {
    return false;
  }
  if (value) {
    *value = addrlog_buf[addrlog_tail];
  }
  addrlog_tail = (addrlog_tail + 1) % EMUL_ADDRLOG_CAPACITY;
  addrlog_count--;
  return true;
}

bool __not_in_flash_func(emul_addrlog_peek)(uint16_t *value) {
  if (addrlog_count == 0 || !value) {
    return false;
  }
  *value = addrlog_buf[addrlog_tail];
  return true;
}

size_t __not_in_flash_func(emul_addrlog_count)(void) { return addrlog_count; }

static void __not_in_flash_func(emul_dma_irqHandlerLookup)(void) {
  uint32_t pending = dma_hw->ints1;
  dma_hw->ints1 = pending;

  while (pending) {
    int chan = __builtin_ctz(pending);
    pending &= ~(1U << chan);

    // Read the address to process
    uint16_t addrLsb = dma_hw->ch[2].al3_read_addr_trig;

    if (addrLsb >= 0xF000) {
      if (addrlog_count < EMUL_ADDRLOG_CAPACITY) {
        addrlog_buf[addrlog_head] = addrLsb;
        addrlog_head = (addrlog_head + 1) % EMUL_ADDRLOG_CAPACITY;
        addrlog_count++;
        // DPRINTF("DMA_LSB LOOKUP: $%x\n", addrLsb);
      }
    }
  }
}

void emul_start() {
  // Copy the target firmware to RAM so the remote machine can execute it.
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length * 4);

  // Initialize the ROM emulator PIO path without command handlers.
  init_romemul(NULL, emul_dma_irqHandlerLookup, false);

  // Initialize the SD card filesystem for the app folder.
  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = "/oric";
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("Error initializing the SD card: %i\n", sdcardErr);
    while (1) {
      sleep_ms(SLEEP_LOOP_MS);
    }
  } else {
    DPRINTF("SD card found & initialized\n");
  }

  // Start the Oric emulation loop.
  DPRINTF("Start the app loop here\n");

  oric_main();
}
