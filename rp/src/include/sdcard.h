/**
 * File: sdcard.h
 * Author: Diego Parrilla Santamaría
 * Date: December 2024
 * Copyright: 2024 - GOODDATA LABS SL
 * Description: Header for sdcard.c which manages the SD card
 */

#ifndef SDCARD_H
#define SDCARD_H

#include "constants.h"
#include "debug.h"
#include "gconfig.h"
#include "sd_card.h"
#include "sdcard.h"

typedef enum {
  SDCARD_INIT_OK = 0,
  SDCARD_INIT_ERROR = -1,
  SDCARD_MOUNT_ERROR = -2,
  SDCARD_CREATE_FOLDER_ERROR = -3
} sdcard_status_t;

#define SDCARD_KILOBAUD 1000

#define NUM_BYTES_PER_SECTOR 512
#define SDCARD_MEGABYTE 1048576

/**
 * @brief Initialize filesystem on SD card.
 *
 * Sets up the SD card filesystem by mounting it and preparing the designated
 * folder. Returns specific status codes reflecting success or the nature of any
 * initialization failure. The folder is created if it does not already exist.
 *
 * @param fsPtr Pointer to a FATFS structure used for filesystem mounting.
 * @param folderName Name of the folder to be created or used on the SD card.
 * @return sdcard_status_t Status code indicating the initialization result.
 */
sdcard_status_t sdcard_initFilesystem(FATFS *fsPtr, const char *folderName);

sdcard_status_t sdcardInit(void);
FRESULT sdcard_mountFilesystem(FATFS *fsys, const char *drive);
bool sdcard_dirExist(const char *dir);
void sdcard_changeSpiSpeed(int baudRateKbits);
void sdcard_setSpiSpeedSettings(void);

#endif  // SDCARD_H
