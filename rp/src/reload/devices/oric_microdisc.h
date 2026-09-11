#pragma once
// WD1793 floppy controller + Oric Microdisc interface.
//
// Ported from Oricutron's disk.c / disk.h:
//
//   Oricutron
//   Copyright (C) 2009-2014 Peter Gordon
//
//   This program is free software; you can redistribute it and/or modify it
//   under the terms of the GNU General Public License as published by the
//   Free Software Foundation, version 2 of the License.
//
//   Additional authors of the ported code (git blame, Oricutron master,
//   2026-09-11): Ivan Sotirov ("iss"), "assinie", "iv.sav.sav", Stefan.
//
// Oricutron is GPL version 2 only. This port is used in md-oric (GPLv3) under
// a specific permission granted by Peter Gordon for the parts md-oric needs:
// https://github.com/pete-gordon/oricutron/issues/216 . The exact functions
// taken are listed in docs/epics/EPIC-07-floppy/IMPORT-LIST.md.
//
// What differs from upstream, and why:
//
//   - The disk image is not held in memory. Upstream loads the whole .dsk
//     (~263 KB); the RP2040 has ~32 KB to spare. One 6400-byte track is read
//     from the microSD card when the head moves (diskimage_cachetrack) and the
//     sector pointer table points into that buffer. READ TRACK / WRITE TRACK
//     index the same buffer.
//   - Writes set a dirty flag on the track buffer, which is flushed back to
//     the file at its offset before another track replaces it and on close.
//   - No `struct machine`. The Microdisc's ROMDIS output and IRQ line are
//     plain fields (`romdis`, `irq`) that the Oric memory map and CPU tick
//     read; the owner remaps $C000-$FFFF when `romdis` / `diskrom` change.
//   - Registers other than $310-$314 and $318 are not handled here; the owner
//     routes them to the VIA, as upstream's default branch did.
//   - MICRODISC_FUDGE (never defined upstream), the Jasmin / Byte Drive 500 /
//     Pravetz controllers, popups and debug output are not carried over.
//   - `last_step_in` lives in the controller struct instead of a file static.
//
// Header-only in md-oric's CHIPS_IMPL style: oric.c defines CHIPS_IMPL once.
//
// Upstream's identifiers (`wd`, `md`, register and status names) are kept
// verbatim so the code can be diffed against Oricutron; hence the lint scope.
// NOLINTBEGIN(readability-identifier-length)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************** MICRODISC *********************/
#define ORIC_MD_MAX_DRIVES 4  // upstream indexes disk[] by c_drive; only 0 used
#define ORIC_MD_TRACK_BYTES 6400
#define ORIC_MD_MAX_SECTORS 32
#define ORIC_MD_ROM_BYTES 0x2000

#define MDSB_INTENA 0
#define MDSF_INTENA (1 << MDSB_INTENA)
#define MDSB_ROMDIS 1
#define MDSF_ROMDIS (1 << MDSB_ROMDIS)
#define MDSB_DENSITY 3
#define MDSF_DENSITY (1 << MDSB_DENSITY)
#define MDSB_SIDE 4
#define MDSF_SIDE (1 << MDSB_SIDE)
#define MDSF_DRIVE 0x60
#define MDSB_EPROM 7  // Write
#define MDSF_EPROM (1 << MDSB_EPROM)
#define MDSB_INTRQ 7  // Read
#define MDSF_INTRQ (1 << MDSB_INTRQ)

#define MB_DRQ 7
#define MF_DRQ (1 << MB_DRQ)

/******************** WD17xx *********************/
#define WSB_BUSY 0
#define WSF_BUSY (1 << WSB_BUSY)
#define WSBI_PULSE 1
#define WSFI_PULSE (1 << WSBI_PULSE)  // Type I only
#define WSB_DRQ 1
#define WSF_DRQ (1 << WSB_DRQ)
#define WSBI_TRK0 2
#define WSFI_TRK0 (1 << WSBI_TRK0)  // Type I only
#define WSB_LOSTDAT 2
#define WSF_LOSTDAT (1 << WSB_LOSTDAT)
#define WSB_CRCERR 3
#define WSF_CRCERR (1 << WSB_CRCERR)
#define WSBI_SEEKERR 4
#define WSFI_SEEKERR (1 << WSBI_SEEKERR)  // Type I only
#define WSB_RNF 4
#define WSF_RNF (1 << WSB_RNF)
#define WSBI_HEADL 5
#define WSFI_HEADL (1 << WSBI_HEADL)  // Type I only
#define WSBR_RECTYP 5
#define WSFR_RECTYP (1 << WSBR_RECTYP)  // Read sector only
#define WSB_WRITEERR 5
#define WSF_WRITEERR (1 << WSB_WRITEERR)
#define WSB_WRPROT 6
#define WSF_WRPROT (1 << WSB_WRPROT)
#define WSB_NOTREADY 7
#define WSF_NOTREADY (1 << WSB_NOTREADY)

enum {
  COP_NUFFINK = 0,    // Not doing anything, guv
  COP_READ_TRACK,     // Reading a track
  COP_READ_SECTOR,    // Reading a sector
  COP_READ_SECTORS,   // Reading multiple sectors
  COP_WRITE_TRACK,    // Writing a track
  COP_WRITE_SECTOR,   // Writing a sector
  COP_WRITE_SECTORS,  // Writing multiple sectors
  COP_READ_ADDRESS    // Reading a sector header
};

typedef struct {
  uint8_t* id_ptr;
  uint8_t* data_ptr;
} oric_mfmsector_t;

// One disk image, open on the card. Only the current track is in memory.
typedef struct {
  bool inserted;
  FIL file;
  bool file_open;
  bool wrprot;
  uint32_t numsides;   // From the MFM_DISK header
  uint32_t numtracks;  // Per side, from the header (the real file layout)
  uint32_t geometry;   // Must be 1: side-major track order
  int16_t cachedtrack;  // Track whose sectors are in sector[] (or -1)
  int16_t cachedside;
  uint32_t numsectors;  // Valid entries in sector[]
  oric_mfmsector_t sector[ORIC_MD_MAX_SECTORS];  // Pointers into track[]
  uint8_t* track;     // ORIC_MD_TRACK_BYTES, supplied by the owner
  int16_t buf_track;  // What track[] currently holds (or -1) -- may differ
  int16_t buf_side;   // from cachedtrack when the track has no sectors
  bool dirty;         // track[] has writes not yet flushed to the file
} oric_diskimage_t;

typedef struct oric_wd17xx {
  uint8_t r_status;  // Status register
  uint8_t r_track;   // Track register
  uint8_t r_sector;  // Sector register
  uint8_t r_data;    // Data register
  uint8_t c_drive;   // Currently active drive
  uint8_t c_side;    // Currently active side
  uint8_t c_track;   // Currently selected track
  uint8_t c_sector;  // Currently selected sector ID
  uint8_t sectype;   // When reading a sector, remembers if it was marked deleted
  bool last_step_in;  // TRUE if the last seek operation stepped the head inwards
  void (*setintrq)(void*);  // Called by the core when INTRQ is set
  void (*clrintrq)(void*);  // Called when INTRQ is cleared
  void* intrqarg;           // Userdata passed to setintrq/clrintrq
  void (*setdrq)(void*);    // Called when DRQ is set
  void (*clrdrq)(void*);    // Called when DRQ is cleared
  void* drqarg;             // Userdata passed to setdrq/clrdrq
  oric_diskimage_t* disk[ORIC_MD_MAX_DRIVES];  // Loaded disk images
  int currentop;                  // Current operation in progress
  oric_mfmsector_t* currsector;   // Sector being used by an active read/write
  int currseclen;                 // The length of the current sector
  int curroffs;                   // Current offset into the above sector
  int delayedint;  // Cycle counter simulating a delay before INTRQ is asserted
  int delayeddrq;  // Cycle counter simulating a delay before DRQ is asserted
  int distatus;  // New r_status when delayedint expires (or -1 to leave it)
  int ddstatus;  // New r_status when delayeddrq expires (or -1 to leave it)
  uint16_t crc;
} oric_wd17xx_t;

typedef struct {
  uint8_t status;  // Status register (write to 0x314)
  uint8_t intrq;   // intrq register (read from 0x314)
  uint8_t drq;     // drq register (read/write 0x318)
  oric_wd17xx_t* wd;
  bool diskrom;  // Microdisc EPROM mapped over $E000-$FFFF (while romdis)
  bool romdis;   // BASIC ROM disabled: $C000-$FFFF is RAM (plus the EPROM)
  bool irq;      // IRQ line to the 6502, already gated by INTENA
} oric_microdisc_t;

// Disk image (adapted from upstream: file-backed, one track resident)
bool diskimage_open(oric_diskimage_t* dimg, const char* path, uint8_t* track);
void diskimage_close(oric_diskimage_t* dimg);
bool diskimage_flush(oric_diskimage_t* dimg);
void diskimage_cachetrack(oric_diskimage_t* dimg, int track, int side);

// WD17xx core
void wd17xx_init(oric_wd17xx_t* wd);
void wd17xx_ticktock(oric_wd17xx_t* wd, int cycles);
void wd17xx_seek_track(oric_wd17xx_t* wd, uint8_t track);
oric_mfmsector_t* wd17xx_find_sector(oric_wd17xx_t* wd, uint8_t secid);
oric_mfmsector_t* wd17xx_first_sector(oric_wd17xx_t* wd);
oric_mfmsector_t* wd17xx_next_sector(oric_wd17xx_t* wd);
uint8_t wd17xx_read(oric_wd17xx_t* wd, uint16_t addr);
void wd17xx_write(oric_wd17xx_t* wd, uint16_t addr, uint8_t data);

// Microdisc interface. Handles $310-$314 and $318; the owner routes the rest
// of $310-$31B to the VIA.
void microdisc_init(oric_microdisc_t* md, oric_wd17xx_t* wd);
uint8_t microdisc_read(oric_microdisc_t* md, uint16_t addr);
void microdisc_write(oric_microdisc_t* md, uint16_t addr, uint8_t data);

#ifdef __cplusplus
}  // extern "C"
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL

// CRC-16 (CCITT) update, one byte at a time.
static uint16_t calc_crc(uint16_t crc, uint8_t value) {
  crc = ((crc << 8) & 0xff00) | ((crc >> 8) & 0x00ff);
  crc ^= value;
  crc ^= (crc & 0xff) >> 4;
  crc ^= (crc << 12) & 0xffff;
  crc ^= (crc & 0xff) << 5;
  return crc;
}

/*-- disk image ------------------------------------------------------------*/

static uint32_t _diskimage_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void _diskimage_forget(oric_diskimage_t* dimg) {
  for (int n = 0; n < ORIC_MD_MAX_SECTORS; n++) {
    dimg->sector[n].id_ptr = NULL;
    dimg->sector[n].data_ptr = NULL;
  }
  dimg->numsectors = 0;
  dimg->cachedtrack = -1;
  dimg->cachedside = -1;
}

// Offset of a track in the file. Geometry 1 is side-major: all of side 0's
// tracks, then side 1's. Upstream normalises every image to 128 tracks in
// memory and relocates side 1 accordingly; reading from the file, the header's
// real track count is what places side 1.
static FSIZE_t _diskimage_track_offset(const oric_diskimage_t* dimg, int track,
                                       int side) {
  return 256u + ((FSIZE_t)side * dimg->numtracks + (FSIZE_t)track) *
                    ORIC_MD_TRACK_BYTES;
}

bool diskimage_flush(oric_diskimage_t* dimg) {
  if (!dimg || !dimg->dirty) {
    return true;
  }
  if (!dimg->file_open || dimg->wrprot || dimg->buf_track < 0) {
    dimg->dirty = false;
    return false;
  }
  UINT written = 0;
  if (f_lseek(&dimg->file,
              _diskimage_track_offset(dimg, dimg->buf_track, dimg->buf_side)) !=
          FR_OK ||
      f_write(&dimg->file, dimg->track, ORIC_MD_TRACK_BYTES, &written) !=
          FR_OK ||
      written != ORIC_MD_TRACK_BYTES) {
    dimg->dirty = false;
    return false;
  }
  (void)f_sync(&dimg->file);
  dimg->dirty = false;
  return true;
}

static bool _diskimage_read_track(oric_diskimage_t* dimg, int track, int side) {
  if (dimg->buf_track == track && dimg->buf_side == side) {
    return true;
  }
  (void)diskimage_flush(dimg);
  dimg->buf_track = -1;
  dimg->buf_side = -1;
  if (!dimg->file_open) {
    return false;
  }
  UINT read = 0;
  if (f_lseek(&dimg->file, _diskimage_track_offset(dimg, track, side)) !=
          FR_OK ||
      f_read(&dimg->file, dimg->track, ORIC_MD_TRACK_BYTES, &read) != FR_OK ||
      read != ORIC_MD_TRACK_BYTES) {
    memset(dimg->track, 0, ORIC_MD_TRACK_BYTES);  // unformatted
    return false;
  }
  dimg->buf_track = (int16_t)track;
  dimg->buf_side = (int16_t)side;
  return true;
}

// Open an MFM_DISK image. Header checks follow upstream diskimage_load.
bool diskimage_open(oric_diskimage_t* dimg, const char* path, uint8_t* track) {
  diskimage_close(dimg);
  memset(dimg, 0, sizeof(*dimg));
  dimg->track = track;
  dimg->buf_track = -1;
  dimg->buf_side = -1;
  _diskimage_forget(dimg);

  FRESULT res = f_open(&dimg->file, path, FA_READ | FA_WRITE);
  if (res != FR_OK) {
    res = f_open(&dimg->file, path, FA_READ);
    if (res != FR_OK) {
      return false;
    }
    dimg->wrprot = true;
  }
  dimg->file_open = true;

  uint8_t hdr[20];
  UINT read = 0;
  if (f_read(&dimg->file, hdr, sizeof(hdr), &read) != FR_OK ||
      read != sizeof(hdr) || memcmp(hdr, "MFM_DISK", 8) != 0) {
    diskimage_close(dimg);
    return false;
  }
  dimg->numsides = _diskimage_le32(&hdr[8]);
  dimg->numtracks = _diskimage_le32(&hdr[12]);
  dimg->geometry = _diskimage_le32(&hdr[16]);
  // Is the disk sane!?
  if (dimg->geometry != 1 || dimg->numsides < 1 || dimg->numsides > 2 ||
      dimg->numtracks < 1 || dimg->numtracks > 128) {
    diskimage_close(dimg);
    return false;
  }
  dimg->inserted = true;
  return true;
}

void diskimage_close(oric_diskimage_t* dimg) {
  if (!dimg) {
    return;
  }
  if (dimg->file_open) {
    (void)diskimage_flush(dimg);
    (void)f_close(&dimg->file);
  }
  dimg->file_open = false;
  dimg->inserted = false;
  dimg->buf_track = -1;
  dimg->buf_side = -1;
  _diskimage_forget(dimg);
}

// Whenever a seek operation occurs, the track where the head ends up is
// "cached": all the sector address and data markers are found, and pointers
// are remembered for each. Here that also means reading the track from the
// card, since only one is ever resident.
void diskimage_cachetrack(oric_diskimage_t* dimg, int track, int side) {
  uint8_t *ptr, *eot;
  uint32_t sectorcount, n;

  // If this track is already cached, don't waste time doing it again
  if ((dimg->cachedtrack == track) && (dimg->cachedside == side)) return;

  // reset cached info
  _diskimage_forget(dimg);

  if ((uint32_t)side >= dimg->numsides) return;
  if ((uint32_t)track >= dimg->numtracks) return;

  // Bring the track in from the card (no-op if the buffer already holds it)
  if (!_diskimage_read_track(dimg, track, side)) return;

  // Find the start and end locations of the track within the buffer
  ptr = dimg->track;
  eot = &ptr[ORIC_MD_TRACK_BYTES];

  // Scan through the track looking for sectors
  sectorcount = 0;
  while (ptr < eot) {
    // Search for ID mark
    while (((ptr + 3) < eot) && !((ptr[0] == 0xa1) && (ptr[1] == 0xa1) &&
                                  (ptr[2] == 0xa1) && (ptr[3] == 0xfe)))
      ptr++;
    ptr += 3;

    // Don't exceed the bounds of this track
    if (ptr >= eot) break;
    if (sectorcount >= ORIC_MD_MAX_SECTORS) break;

    // Store ID pointer
    dimg->sector[sectorcount].id_ptr = ptr;
    dimg->sector[sectorcount].data_ptr = NULL;
    sectorcount++;

    // Get N value
    n = ptr[4];

    // Skip ID field and CRC
    ptr += 7;

    // Search for data ID
    while ((ptr < eot) && (ptr[0] != 0xfb) && (ptr[0] != 0xf8)) ptr++;
    if (ptr >= eot) break;

    // Store pointer
    dimg->sector[sectorcount - 1].data_ptr = ptr;

    // Skip data field and ID
    ptr += (1 << ((n & 3) + 7)) + 3;
  }

  if (0 < sectorcount) {
    // Remember how many sectors we have successfully cached
    dimg->numsectors = sectorcount;
    dimg->cachedtrack = (int16_t)track;
    dimg->cachedside = (int16_t)side;
  }
}

static void diskimage_set_modified(oric_diskimage_t* dimg) {
  if (dimg) {
    dimg->dirty = true;
  }
}

/*-- WD17xx ---------------------------------------------------------------*/

// This routine does nothing. It is just used as a default for the callback
// routines and is replaced by the microdisc implementation.
static void wd17xx_dummy(void* nothing) { (void)nothing; }

// Initialise a WD17xx controller instance
void wd17xx_init(oric_wd17xx_t* wd) {
  wd->r_status = 0;
  wd->r_track = 0;
  wd->r_sector = 0;
  wd->r_data = 0;
  wd->c_drive = 0;
  wd->c_side = 0;
  wd->c_track = 0;
  wd->c_sector = 0;
  wd->last_step_in = false;
  wd->setintrq = wd17xx_dummy;
  wd->clrintrq = wd17xx_dummy;
  wd->intrqarg = NULL;
  wd->setdrq = wd17xx_dummy;
  wd->clrdrq = wd17xx_dummy;
  wd->drqarg = NULL;
  wd->currentop = COP_NUFFINK;
  wd->currsector = NULL;
  wd->delayedint = 0;
  wd->delayeddrq = 0;
  wd->distatus = -1;
  wd->ddstatus = -1;
}

// This routine emulates some cycles of disk activity.
void wd17xx_ticktock(oric_wd17xx_t* wd, int cycles) {
  // Is there a pending INTRQ?
  if (wd->delayedint > 0) {
    // Count down the INTRQ timer!
    wd->delayedint -= cycles;

    // Time to assert INTRQ?
    if (wd->delayedint <= 0) {
      // Yep! Stop timing.
      wd->delayedint = 0;

      // Need to update the status register?
      if (wd->distatus != -1) {
        // Yep. Do so.
        wd->r_status = (uint8_t)wd->distatus;
        wd->distatus = -1;
      }

      // Assert INTRQ (this function pointer is set up by the microdisc)
      wd->setintrq(wd->intrqarg);
    }
  }

  // Is there a pending DRQ?
  if (wd->delayeddrq > 0) {
    // Count down the DRQ timer!
    wd->delayeddrq -= cycles;

    // Time to assert DRQ?
    if (wd->delayeddrq <= 0) {
      // Yep! Stop timing.
      wd->delayeddrq = 0;

      // Need to update the status register?
      if (wd->ddstatus != -1) {
        // Yep. Do so.
        wd->r_status = (uint8_t)wd->ddstatus;
        wd->ddstatus = -1;
      }

      // Assert DRQ
      wd->r_status |= WSF_DRQ;
      wd->setdrq(wd->drqarg);
    }
  }
}

// This routine seeks to the specified track. It is used by the SEEK and STEP
// commands.
void wd17xx_seek_track(oric_wd17xx_t* wd, uint8_t track) {
  oric_diskimage_t* dimg = wd->disk[wd->c_drive];

  // Is there a disk in the drive?
  if (dimg && dimg->inserted) {
    // Yes. If we are trying to seek to a non-existent track, just seek as far
    // as we can
    if (track >= dimg->numtracks) {
      track = (uint8_t)((dimg->numtracks > 0) ? dimg->numtracks - 1 : 0);
      wd->distatus = WSFI_HEADL | WSFI_SEEKERR;
    } else {
      wd->distatus = WSFI_HEADL | WSFI_PULSE;
    }

    // Cache the new track
    diskimage_cachetrack(dimg, track, wd->c_side);

    // Update our status
    wd->c_track = track;
    wd->c_sector = 0;
    wd->r_track = track;

    // Assert INTRQ in 20 cycles time and update the status accordingly
    // (note: 20 cycles is waaaaaay faster than any real drive could seek. The
    // actual delay would depend how far the head had to seek, and what
    // stepping speed was currently set).
    wd->delayedint = 20;
    if (wd->c_track == 0) wd->distatus |= WSFI_TRK0;
    return;
  }

  // No disk in drive
  // Set INTRQ because the operation has finished.
  wd->setintrq(wd->intrqarg);
  wd->r_track = 0;

  // Set error state
  wd->r_status = WSF_NOTREADY | WSFI_SEEKERR;
}

// This routine looks for the sector with the specified ID in the current
// track. It returns NULL if there is no such sector, or a structure with
// pointers to the ID and data fields if the sector is found.
oric_mfmsector_t* wd17xx_find_sector(oric_wd17xx_t* wd, uint8_t secid) {
  int revs = 0;
  oric_diskimage_t* dimg;

  // Save some typing...
  dimg = wd->disk[wd->c_drive];

  // No disk image? No sectors!
  if (!dimg || !dimg->inserted) return NULL;

  // Make sure the current track is cached
  diskimage_cachetrack(dimg, wd->c_track, wd->c_side);

  // No sectors on this track? Someone needs to format their disk...
  if (dimg->numsectors < 1) return NULL;

  // We do this more realistically than we need to since this is not
  // a super-accurate emulation (for now). Never mind. Lets go
  // around the track up to two times.
  while (revs < 2) {
    // Move on to the next sector
    wd->c_sector = (uint8_t)((wd->c_sector + 1) % dimg->numsectors);

    // If we passed through the start of the track, set the pulse bit in the
    // status register
    if (!wd->c_sector) {
      revs++;
      wd->r_status |= WSFI_PULSE;
    }

    // Found the required sector?
    if (dimg->sector[wd->c_sector].id_ptr[3] == secid)
      return &dimg->sector[wd->c_sector];
  }

  // The search failed :-(
  return NULL;
}

// This returns the first valid sector in the current track, or NULL if
// there aren't any sectors.
oric_mfmsector_t* wd17xx_first_sector(oric_wd17xx_t* wd) {
  oric_diskimage_t* dimg;

  dimg = wd->disk[wd->c_drive];

  // No disk? no sector...
  if (!dimg || !dimg->inserted) return NULL;

  // Make sure the current track is cached
  diskimage_cachetrack(dimg, wd->c_track, wd->c_side);

  // No sectors?!
  if (dimg->numsectors < 1) return NULL;

  // We're at the first sector!
  wd->c_sector = 0;
  wd->r_status = WSFI_PULSE;

  // Return the sector pointers
  return &dimg->sector[wd->c_sector];
}

// Move on to the next sector
oric_mfmsector_t* wd17xx_next_sector(oric_wd17xx_t* wd) {
  oric_diskimage_t* dimg;

  dimg = wd->disk[wd->c_drive];

  // No disk? No sectors!
  if (!dimg || !dimg->inserted) return NULL;

  // Make sure the current track is cached
  diskimage_cachetrack(dimg, wd->c_track, wd->c_side);

  // No sectors?
  if (dimg->numsectors < 1) return NULL;

  // Get the next sector number
  wd->c_sector = (uint8_t)((wd->c_sector + 1) % dimg->numsectors);

  // If we are at the start of the track, set the pulse bit
  if (!wd->c_sector) wd->r_status |= WSFI_PULSE;

  // Return the sector pointers
  return &dimg->sector[wd->c_sector];
}

// The track buffer for READ TRACK / WRITE TRACK. Upstream indexed the whole
// image directly; here the current track has to be resident first.
static uint8_t* _wd17xx_track_buffer(oric_wd17xx_t* wd) {
  oric_diskimage_t* dimg = wd->disk[wd->c_drive];
  if (!dimg || !dimg->inserted) return NULL;
  diskimage_cachetrack(dimg, wd->c_track, wd->c_side);
  if (dimg->buf_track != wd->c_track || dimg->buf_side != wd->c_side) {
    return NULL;
  }
  return dimg->track;
}

// Perform a read operation on a WD17xx register
uint8_t wd17xx_read(oric_wd17xx_t* wd, uint16_t addr) {
  // Which register?!
  switch (addr) {
    case 0:  // Status register
      wd->clrintrq(wd->intrqarg);  // Reading the status register clears INTRQ
      return wd->r_status;

    case 1:  // Track register
      return wd->r_track;

    case 2:  // Sector register
      return wd->r_sector;

    case 3:  // Data register
      // What are we currently doing?
      switch (wd->currentop) {
        case COP_READ_SECTOR:
        case COP_READ_SECTORS:
          // We somehow started a sector read operation without a valid sector.
          if (!wd->currsector) {
            // Abort.
            wd->r_status &= ~WSF_DRQ;
            wd->r_status |= WSF_RNF;
            wd->clrdrq(wd->drqarg);
            wd->currentop = COP_NUFFINK;
            break;
          }

          // If this is the first read of a read operation, remember the record
          // type for later
          if (wd->curroffs == 0)
            wd->sectype = (wd->currsector->data_ptr[wd->curroffs++] == 0xf8)
                              ? WSFR_RECTYP
                              : 0x00;

          // Get the next byte from the sector
          wd->r_data = wd->currsector->data_ptr[wd->curroffs++];
          wd->crc = calc_crc(wd->crc, wd->r_data);

          // Clear any previous DRQ
          wd->r_status &= ~WSF_DRQ;
          wd->clrdrq(wd->drqarg);

          // Has the whole sector been read?
          if (wd->curroffs > wd->currseclen) {
            // We've got to the end of the current sector. IF it is a multiple
            // sector operation, we need to move on!
            if (wd->currentop == COP_READ_SECTORS) {
              // Get the next sector, and carry on!
              wd->r_sector++;
              wd->curroffs = 0;
              wd->currsector = wd17xx_find_sector(wd, wd->r_sector);
              wd->crc = 0xe295;

              // If we hit the end of the track, that's fine, it just means the
              // operation is finished.
              if (!wd->currsector) {
                wd->delayedint = 20;  // Assert INTRQ in 20 cycles time
                wd->distatus = wd->sectype;  // ...and set the record type
                wd->currentop = COP_NUFFINK;  // No longer in an operation
                wd->r_status &= (~WSF_DRQ);   // Clear DRQ (no data to read)
                wd->clrdrq(wd->drqarg);
                break;
              }

              // We've got the next sector lined up. Assert DRQ in 180 cycles
              // time (simulate a bit of a delay between sectors. Note that most
              // of these values have been pulled out of thin air and might
              // need adjusting for some pickier loaders).
              wd->delayeddrq = 180;
              break;
            }

            // Just reading one sector so..
            wd->delayedint = 32;  // INTRQ in a little while because we're done
            wd->distatus = wd->sectype;   // Set the status accordingly
            wd->currentop = COP_NUFFINK;  // Finished the op
            wd->r_status &= (~WSF_DRQ);   // Clear DRQ (no more data)
            wd->clrdrq(wd->drqarg);
          } else {
            wd->delayeddrq = 32;  // More data ready. DRQ to let them know!
          }
          break;

        case COP_READ_TRACK: {
          uint8_t* trk = _wd17xx_track_buffer(wd);
          wd->r_data = trk ? trk[wd->curroffs] : 0;
          wd->curroffs++;
          wd->r_status &= ~WSF_DRQ;
          wd->clrdrq(wd->drqarg);

          // Has the whole track been read?
          if (wd->curroffs >= ORIC_MD_TRACK_BYTES) {
            wd->delayedint = 20;
            wd->distatus = 0;
            wd->currentop = COP_NUFFINK;
          } else {
            wd->delayeddrq = 32;
          }
          break;
        }

        case COP_READ_ADDRESS:
          if (!wd->currsector) {
            wd->r_status &= ~WSF_DRQ;
            wd->clrdrq(wd->drqarg);
            wd->currentop = COP_NUFFINK;
            break;
          }
          if (wd->curroffs == 0) wd->r_sector = wd->currsector->id_ptr[1];
          wd->r_data = wd->currsector->id_ptr[++wd->curroffs];
          wd->r_status &= ~WSF_DRQ;
          wd->clrdrq(wd->drqarg);
          if (wd->curroffs >= 6) {
            wd->delayedint = 20;
            wd->distatus = 0;
            wd->currentop = COP_NUFFINK;
          } else {
            wd->delayeddrq = 32;
          }
          break;
      }
      return wd->r_data;
  }

  return 0;  // ??
}

// Perform a write operation on a WD17xx register
void wd17xx_write(oric_wd17xx_t* wd, uint16_t addr, uint8_t data) {
  switch (addr) {
    case 0:  // Command register
      wd->clrintrq(wd->intrqarg);
      switch (data & 0xe0) {
        case 0x00:  // Restore or seek
          switch (data & 0x10) {
            case 0x00:  // Restore (Type I)
              wd->r_status = WSF_BUSY;
              if (data & 8) wd->r_status |= WSFI_HEADL;
              wd17xx_seek_track(wd, 0);
              wd->currentop = COP_NUFFINK;
              break;

            case 0x10:  // Seek (Type I)
              wd->r_status = WSF_BUSY;
              if (data & 8) wd->r_status |= WSFI_HEADL;
              wd17xx_seek_track(wd, wd->r_data);
              wd->currentop = COP_NUFFINK;
              break;
          }
          break;

        case 0x20:  // Step (Type I)
          wd->r_status = WSF_BUSY;
          if (data & 8) wd->r_status |= WSFI_HEADL;
          if (wd->last_step_in)
            wd17xx_seek_track(wd, wd->c_track + 1);
          else
            wd17xx_seek_track(wd, wd->c_track > 0 ? wd->c_track - 1 : 0);
          wd->currentop = COP_NUFFINK;
          break;

        case 0x40:  // Step-in (Type I)
          wd->r_status = WSF_BUSY;
          if (data & 8) wd->r_status |= WSFI_HEADL;
          wd17xx_seek_track(wd, wd->c_track + 1);
          wd->last_step_in = true;
          wd->currentop = COP_NUFFINK;
          break;

        case 0x60:  // Step-out (Type I)
          wd->r_status = WSF_BUSY;
          if (data & 8) wd->r_status |= WSFI_HEADL;
          if (wd->c_track > 0) wd17xx_seek_track(wd, wd->c_track - 1);
          wd->last_step_in = false;
          wd->currentop = COP_NUFFINK;
          break;

        case 0x80:  // Read sector (Type II)
          wd->curroffs = 0;
          wd->currsector = wd17xx_find_sector(wd, wd->r_sector);
          if (!wd->currsector) {
            wd->r_status = WSF_RNF;
            wd->clrdrq(wd->drqarg);
            wd->setintrq(wd->intrqarg);
            wd->currentop = COP_NUFFINK;
            break;
          }
          wd->currseclen = 1 << ((wd->currsector->id_ptr[4] & 3) + 7);
          wd->r_status = WSF_BUSY | WSF_NOTREADY;
          wd->delayeddrq = 60;
          wd->currentop = (data & 0x10) ? COP_READ_SECTORS : COP_READ_SECTOR;
          wd->crc = 0xe295;
          break;

        case 0xa0:  // Write sector (Type II)
          wd->curroffs = 0;
          wd->currsector = wd17xx_find_sector(wd, wd->r_sector);
          if (!wd->currsector) {
            wd->r_status = WSF_RNF;
            wd->clrdrq(wd->drqarg);
            wd->setintrq(wd->intrqarg);
            wd->currentop = COP_NUFFINK;
            break;
          }
          wd->currseclen = 1 << ((wd->currsector->id_ptr[4] & 3) + 7);
          wd->r_status = WSF_BUSY | WSF_NOTREADY;
          wd->delayeddrq = 500;
          wd->currentop = (data & 0x10) ? COP_WRITE_SECTORS : COP_WRITE_SECTOR;
          wd->crc = 0xe295;
          break;

        case 0xc0:  // Read address / Force IRQ
          switch (data & 0x10) {
            case 0x00:  // Read address (Type III)
              wd->curroffs = 0;
              if (!wd->currsector)
                wd->currsector = wd17xx_first_sector(wd);
              else
                wd->currsector = wd17xx_next_sector(wd);

              if (!wd->currsector) {
                wd->r_status = WSF_RNF;
                wd->clrdrq(wd->drqarg);
                wd->currentop = COP_NUFFINK;
                wd->setintrq(wd->intrqarg);
                break;
              }

              wd->r_status = WSF_NOTREADY | WSF_BUSY | WSF_DRQ;
              wd->setdrq(wd->drqarg);
              wd->currentop = COP_READ_ADDRESS;
              break;

            case 0x10:  // Force Interrupt (Type IV)
              wd->r_status = 0;
              wd->clrdrq(wd->drqarg);
              wd->setintrq(wd->intrqarg);
              wd->delayedint = 0;
              wd->delayeddrq = 0;
              wd->currentop = COP_NUFFINK;
              break;
          }
          break;

        case 0xe0:  // Read track / Write track
          switch (data & 0x10) {
            case 0x00:  // Read track (Type III)
              wd->curroffs = 0;
              wd->r_status = WSF_BUSY | WSF_NOTREADY;
              wd->delayeddrq = 60;
              wd->currentop = COP_READ_TRACK;
              break;

            case 0x10:  // Write track (Type III)
              wd->curroffs = 0;
              wd->r_status = WSF_NOTREADY | WSF_BUSY;
              wd->delayeddrq = 500;
              wd->clrdrq(wd->drqarg);
              wd->clrintrq(wd->intrqarg);
              wd->delayedint = 0;
              wd->currentop = COP_WRITE_TRACK;
              break;
          }
          break;
      }
      break;

    case 1:  // Track register
      wd->r_track = data;
      break;

    case 2:  // Sector register
      wd->r_sector = data;
      break;

    case 3:  // Data register
      wd->r_data = data;
      switch (wd->currentop) {
        case COP_WRITE_SECTOR:
        case COP_WRITE_SECTORS:
          if (!wd->currsector) {
            wd->r_status &= ~WSF_DRQ;
            wd->r_status |= WSF_RNF;
            wd->clrdrq(wd->drqarg);
            wd->currentop = COP_NUFFINK;
            break;
          }

          if (wd->curroffs == 0) wd->currsector->data_ptr[wd->curroffs++] = 0xfb;
          wd->currsector->data_ptr[wd->curroffs++] = wd->r_data;
          wd->crc = calc_crc(wd->crc, wd->r_data);
          diskimage_set_modified(wd->disk[wd->c_drive]);

          wd->r_status &= ~WSF_DRQ;
          wd->clrdrq(wd->drqarg);

          if (wd->curroffs > wd->currseclen) {
            wd->currsector->data_ptr[wd->curroffs++] = (uint8_t)(wd->crc >> 8);
            wd->currsector->data_ptr[wd->curroffs++] = (uint8_t)wd->crc;

            if (wd->currentop == COP_WRITE_SECTORS) {
              // Get the next sector, and carry on!
              wd->r_sector++;
              wd->curroffs = 0;
              wd->currsector = wd17xx_find_sector(wd, wd->r_sector);
              wd->crc = 0xe295;
              if (!wd->currsector) {
                wd->delayedint = 20;
                wd->distatus = wd->sectype;
                wd->currentop = COP_NUFFINK;
                wd->r_status &= (~WSF_DRQ);
                wd->clrdrq(wd->drqarg);
                break;
              }

              wd->delayeddrq = 180;
              break;
            }

            wd->delayedint = 32;
            wd->distatus = wd->sectype;
            wd->currentop = COP_NUFFINK;
            wd->r_status &= (~WSF_DRQ);
            wd->clrdrq(wd->drqarg);
          } else {
            wd->delayeddrq = 32;
          }
          break;

        case COP_WRITE_TRACK: {
          uint8_t* trk = _wd17xx_track_buffer(wd);
          switch (data) {
            // All bytes > 0xF4 are control bytes
            case 0xf5:
              // MFM: Initialize CRC generator
              // FM : Not allowed
              wd->crc = 0x968b;
              wd->r_data = 0xa1;
              wd->crc = calc_crc(wd->crc, wd->r_data);
              break;
            case 0xf6:
              // FM : Not allowed
              wd->r_data = 0xc2;
              wd->crc = calc_crc(wd->crc, wd->r_data);
              break;
            case 0xf7:
              if (trk && wd->curroffs < ORIC_MD_TRACK_BYTES)
                trk[wd->curroffs] = (uint8_t)(wd->crc >> 8);
              wd->curroffs++;
              wd->r_data = wd->crc & 0xff;
              break;
            // MFM: 0xf8 -> 0xff: write control byte
            // FM : 0xf8 -> 0xfe: Initialize CRC generator
            default:
              wd->crc = calc_crc(wd->crc, wd->r_data);
              break;
          }

          // Write byte to disk image
          if (trk && wd->curroffs < ORIC_MD_TRACK_BYTES)
            trk[wd->curroffs] = wd->r_data;
          wd->curroffs++;
          wd->r_status &= ~WSF_DRQ;
          wd->clrdrq(wd->drqarg);
          diskimage_set_modified(wd->disk[wd->c_drive]);

          // Has the whole track been written?
          if (wd->curroffs >= ORIC_MD_TRACK_BYTES) {
            wd->delayedint = 32;
            wd->currentop = COP_NUFFINK;
            wd->r_status = 0;
            wd->clrdrq(wd->drqarg);
            // The sector table now describes stale data
            if (wd->disk[wd->c_drive]) _diskimage_forget(wd->disk[wd->c_drive]);
          } else {
            wd->delayeddrq = 64;
          }
          break;
        }
      }
      break;
  }
}

/*-- Microdisc ------------------------------------------------------------*/

static void microdisc_setdrq(void* md) {
  oric_microdisc_t* mdp = (oric_microdisc_t*)md;
  mdp->drq = 0;
}

static void microdisc_clrdrq(void* md) {
  oric_microdisc_t* mdp = (oric_microdisc_t*)md;
  mdp->drq = MF_DRQ;
}

static void microdisc_setintrq(void* md) {
  oric_microdisc_t* mdp = (oric_microdisc_t*)md;
  mdp->intrq = 0;
  if (mdp->status & MDSF_INTENA) mdp->irq = true;
}

static void microdisc_clrintrq(void* md) {
  oric_microdisc_t* mdp = (oric_microdisc_t*)md;
  mdp->intrq = MDSF_INTRQ;
  mdp->irq = false;
}

void microdisc_init(oric_microdisc_t* md, oric_wd17xx_t* wd) {
  wd17xx_init(wd);
  wd->setintrq = microdisc_setintrq;
  wd->clrintrq = microdisc_clrintrq;
  wd->intrqarg = (void*)md;
  wd->setdrq = microdisc_setdrq;
  wd->clrdrq = microdisc_clrdrq;
  wd->drqarg = (void*)md;
  md->status = 0;
  md->intrq = 0;
  md->drq = 0;
  md->wd = wd;
  md->diskrom = true;
  md->romdis = false;  // the owner decides at reset (D-17)
  md->irq = false;
}

uint8_t microdisc_read(oric_microdisc_t* md, uint16_t addr) {
  if ((addr >= 0x310) && (addr < 0x314)) return wd17xx_read(md->wd, addr & 3);

  switch (addr) {
    case 0x314:
      return md->intrq | 0x7f;

    case 0x318:
      return md->drq | 0x7f;

    default:
      break;
  }

  return 0xff;  // not ours; the owner routes these to the VIA
}

void microdisc_write(oric_microdisc_t* md, uint16_t addr, uint8_t data) {
  if ((addr >= 0x310) && (addr < 0x314)) {
    wd17xx_write(md->wd, addr & 3, data);
    return;
  }

  switch (addr) {
    case 0x314:
      md->status = data;

      // Interrupts enabled, and /INTRQ == 0 ?
      if ((data & MDSF_INTENA) && (md->intrq == 0)) {
        md->irq = true;
      } else {
        md->irq = false;
      }

      md->wd->c_drive = (data & MDSF_DRIVE) >> 5;
      md->wd->c_side = (data & MDSF_SIDE) ? 1 : 0;
      md->romdis = (data & MDSF_ROMDIS) ? false : true;
      md->diskrom = (data & MDSF_EPROM) ? false : true;
      break;

    case 0x318:
      md->drq = (data & MF_DRQ);
      break;

    default:
      break;
  }
}

#endif  // CHIPS_IMPL
// NOLINTEND(readability-identifier-length)
