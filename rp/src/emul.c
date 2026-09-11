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

// How often to look for a card once the Atari has been sent back to GEM.
#define SDCARD_RETRY_MS 500

void emul_start() {
  // Copy the target firmware to RAM so the remote machine can execute it.
  // The macro's length is in bytes; target_firmware_length counts uint16_t
  // entries. Asking for length * 4 copied twice the image -- 1360 bytes of
  // cart code plus 1360 bytes of whatever follows it in flash, landing on
  // everything from offset 1360 up, the listener longword at $05F8 included.
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware,
                       target_firmware_length * sizeof(target_firmware[0]));

  // Tell the cartridge code it may run. Written before romemul starts
  // serving the bus, so the Atari can never read an uninitialised value
  // here; the SD check below sets it to NO_SDCARD if there is nothing to
  // load, long before the Atari gets as far as reading it.
  volatile uint16_t *bootStatus =
      (volatile uint16_t *)((uint8_t *)&__rom_in_ram_start__ +
                            ATARI_ST_BOOTSTATUS_OFFSET);
  *bootStatus = ATARI_ST_BOOT_OK;

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
    // No card means no ROM, so there is nothing to emulate. Tell the
    // cartridge code, which says so and hands back to GEM. Then keep
    // looking, so that inserting a card and resetting the Atari works
    // without power-cycling the board as well.
    DPRINTF("Error initializing the SD card: %i\n", sdcardErr);
    *bootStatus = ATARI_ST_BOOT_NO_SDCARD;
    while (sdcardErr != SDCARD_INIT_OK) {
      sleep_ms(SDCARD_RETRY_MS);
      sdcardErr = sdcard_initFilesystem(&fsys, folderName);
    }
    *bootStatus = ATARI_ST_BOOT_OK;
  }
  DPRINTF("SD card found & initialized\n");

  // Start the Oric emulation loop.
  DPRINTF("Start the app loop here\n");

  oric_main();
}
