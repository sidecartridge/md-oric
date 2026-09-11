/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025
 * Copyright: 2025 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"
#include "commemul.h"

#include "reload/systems/oric/src/oric.h"

// Include the target firmware binary.
#include "target_firmware.h"

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// Blit-finished signal from the m68k, counted by the ROM3 dispatch in
// oric.c; Core 1 paces on it.
volatile uint32_t __not_in_flash() emul_blitDoneCount = 0;

void emul_start() {
  // Copy the target firmware to RAM so the remote machine can execute it.
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length * 4);

  // ROM4 serves the cartridge image and framebuffers; nothing on the RP
  // needs to see those reads, so no DMA interrupt is installed at all.
  init_romemul(NULL, NULL, false);

  // ROM3 is the m68k -> RP signalling window: every read of $FBxxxx is
  // sampled by its own PIO state machine straight into a 32 KB DMA ring,
  // which Core 0 drains. No interrupt, so two back-to-back reads can never
  // overwrite each other -- the way the old per-read DMA IRQ lost key
  // events (EPIC-08 STORY-01). Ported from md-framebuffer-template.
  if (commemul_init() < 0) {
    panic("commemul_init failed");
  }

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
