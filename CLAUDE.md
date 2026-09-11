# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `README.md` (user-facing install/usage notes), `AGENTS.md` (overlapping playbook: layout, build commands, code style, workflow rules), and `CHANGELOG.md` (release history).

## What this repo is

A **SidecarTridge Multi-device microfirmware app that emulates an Oric 1 / Oric Atmos** on Atari ST / STE / MegaST(E) hardware. The whole emulator — 6502 CPU, 6522 VIA, AY-3-8912 PSG, memory map, tape drive — runs on the Raspberry Pi Pico (RP2040) plugged into the Multi-device cartridge slot. The Atari ST is reduced to a display, a keyboard and a sound chip: a small m68k program in the emulated cartridge ROM copies a ready-made planar framebuffer from the cartridge window to ST screen memory every VBL, forwards IKBD scancodes back to the RP over the cartridge bus, and replays queued AY register writes to the ST's own YM2149.

The emulator core is a port of [Reload Emulator](https://github.com/vsladkov/reload-emulator) by Veselin Sladkov, reorganised under `rp/src/reload/`.

**Two moving parts, and almost all the work is on the RP2040 side:**
- **RP2040 Core 0** runs the Oric at 1 MHz (`oric_tick` × 19968 per frame) and drains the keyboard ring.
- **RP2040 Core 1** converts the Oric's 240×224 text/hires screen into ST planar 3-bitplane form at ~50 Hz.
- **Atari ST m68k** does a fixed unrolled copy of 224×90 bytes into one of two screen pages, flips the video base, drains the AY queue, and polls the IKBD ACIA from a Timer-B interrupt.

Low resolution only — the cartridge init aborts to GEM with a message on any other `Getrez()` result.

Public build/usage docs: <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb
# <build_type> = debug | release   (note: CMake is always invoked as Debug — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 4f6e8575-1590-4660-95ad-e6167e46715e
```

Required host environment:
- ARM GNU Toolchain (`arm-none-eabi-*`) and CMake 3.26+.
- `atarist-toolkit-docker` (`stcmd`) — needed for the m68k target. `stcmd` requires a PTY; CI strips the `-it` from `/usr/local/bin/stcmd` to work around it.
- Python 3 (for `target/atarist/firmware.py`).
- SDK paths (auto-set from the repo if unset): `PICO_SDK_PATH`, `FATFS_SDK_PATH`.

Build flow (orchestrated by `build.sh`):
1. Copies `version.txt` into `rp/` and `target/`.
2. Builds the Atari ST target (`target/atarist/build.sh` → `stcmd make release`), producing `BOOT.BIN`. A copy (`FIRMWARE.IMG`) is padded to 64 KB, then `firmware.py` **trims the trailing zeros** and emits `rp/src/include/target_firmware.h` — a `uint16_t` C array embedded in the RP firmware. Current cart image is ~1.3 KB.
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, fatfs-sdk v3.6.2), runs CMake, produces `rp/dist/rp-<board>.uf2`. The FatFs configuration lives at `rp/src/ff/ffconf.h` (project-owned; `rp/src/CMakeLists.txt` puts that directory on the include path) so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

### Build gotchas
- **`rp/build.sh` always runs `cmake ../src -DCMAKE_BUILD_TYPE=Debug`** regardless of the `<build_type>` argument; the `$BUILD_TYPE` line is left commented next to it. `<build_type>` only controls the `DEBUG_MODE` macro (which gates UART stdio) and the dist filename.
- **`pico-extras` is not a submodule here** (the pin and the `pico_extras_import.cmake` include are both commented out), even though `rp/src/CMakeLists.txt` still defaults a `PICO_EXTRAS_PATH` env var. Don't "fix" the missing directory — nothing links against it.
- Both `rp/build.sh` and the root `build.sh` **delete `build/` and `dist/` and re-pin the submodule tags** on every run. Don't invoke them casually; compile directly against the existing `rp/build/` tree to verify a change.
- If you change anything under `target/`, the embedded `rp/src/include/target_firmware.h` is stale until you rebuild the target. A stale header gives you a working RP firmware that shows garbage on the ST.
- Harmless VASM warnings during the m68k build can be ignored. `stcmd` errors like `the input device is not a TTY` mean it was invoked without a PTY.

### CI / release
- `.github/workflows/build.yml` builds `pico_w` Release on PR.
- `.github/workflows/release.yml` triggers on `v*` tags: builds, attaches UF2 + JSON to the GitHub Release, and uploads to the `atarist.sidecartridge.com` S3 bucket.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag (which triggers release).

### Tests
There is no test suite. "Verification" is: build succeeds, UF2 boots on hardware, manual interaction plus the serial debug console (`DPRINTF`, debug builds only).

## Architecture

A **two-target build**: m68k assembly that runs on the Atari ST is assembled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Frame pipeline (RP → ST)

1. **Core 1 (`core1_main` in `oric.c`)** wakes every 19968 µs and calls `oric_screen_update(&state.oric)` — or `oric_show_msg` while a transient "Loading Fn file…" message is active. That function walks the Oric's 240×224 screen, expands attributes/pattern bits through `oric_pat_lut`, and writes **ST planar 3-bitplane** data straight into the cartridge window at offset `0x1000`.
2. **Core 1 then publishes the frame counter** as a `uint16_t` at offset `0x0FFC` of the window. The counter is both the dirty signal and, in bit 0, the identity of the framebuffer holding the frame (`_oric_fb_for_count`). The ST *page* (`$60000`/`$70000`) is separate and chosen by the m68k.
3. **The m68k VBL loop** (`.loop_low_st` in `main.s`) waits for the VBL flag, drains the AY queue, then compares the word at `FRAMECOUNT_ADDR` against its saved copy. Unchanged → go back to sleep. Changed → run the unrolled copy loop.
4. **The copy loop** does 224 iterations of a fully unrolled `move.l`/`move.w` block — 15 longword+word pairs = 90 bytes per line — from `FRAMEBUFFER_A_ADDR` or `FRAMEBUFFER_B_ADDR` (selected by `btst #0` on the counter, outside the loop) into `SCREEN_A_BASE_ADDR + CENTERED_XPOS` (`$60000 + 16`) or `SCREEN_B_BASE_ADDR + CENTERED_XPOS` (`$70000 + 16`), advancing the destination by 160 bytes (a full ST line) each row. The 16-byte X offset centres the 240-pixel Oric screen in the 320-pixel ST line.
5. **The m68k flips the video base** (`$FFFF8201`/`$8203`) to the page it just filled, then alternates `.page_flag` so the next blit targets the other page. Page selection is entirely the m68k's — it deliberately does *not* derive from the RP's counter, because a value-derived page dropped a frame whenever the RP completed two frames inside one ST frame. One frame of display latency is inherent here: the ST cannot display the cartridge framebuffer directly, so the blit must finish before the shifter can show it.

There is **no chunky→planar step on the RP** — Core 1 writes ST planar data directly. The cartridge framebuffer is double-buffered (A at `$FA1000`, B at `$FA8000`): Core 1 renders into the buffer named by bit 0 of the counter it is about to publish, so it normally never writes the one the m68k is blitting. Not yet fully tear-proof — Core 1 renders every 19968 µs and the m68k blits every ~20000 µs, so Core 1 can occasionally lap and reclaim a buffer still being read; closing that needs the blit-finished handshake (D-10).

**Cross-core data needs barriers (D-13).** `oric_msg_buf` is written by Core 0 and read by Core 1; without the `__dmb()` pair in `oric_set_loading_msg` / `core1_main` the compiler may cache the first byte in a register and silently skip the overlay forever. Any new Core 0 ↔ Core 1 signal must follow the same flag-plus-payload pattern.

### Keyboard pipeline (ST → RP)

1. **MFP Timer-B**, in event-count mode on HBL with `TIMERB_COUNT_SCAN_LINES = 10`, fires `.timerb_routine` roughly every 10 scanlines. TOS's HBL / Timer-A / Timer-C / Timer-D / ACIA vectors are all pointed at a bare `rte` and the MFP timers are disabled+masked, so nothing else interrupts the copy loop.
2. **The handler polls the IKBD ACIA** at `$FFFFFC00`: if RX-ready, it reads `$FFFFFC02` and forwards the byte with **two dummy cart reads** — `tst.b (ROMCMD_START_ADDR + CMD_KEYPRESS|CMD_KEYRELEASE)` followed by `tst.b (a0, d0.w)` — i.e. a command read at `$FAF000 + $0BCD` / `$FAF000 + $0CBA`, then a payload read at `$FAF000 + scancode`. Bit 7 of the ACIA byte selects press vs release. Mouse reporting is turned off at boot (`$12`); joysticks are not handled.
3. **RP capture** — `init_romemul(NULL, emul_dma_irqHandlerLookup, false)` in `emul.c` installs a DMA-completion IRQ on the lookup channel. The handler reads back the DMA read address, keeps the low 16 bits, and pushes any value `>= 0xF000` into a 16-entry ring (`emul_addrlog_*`). Everything below that — framebuffer and AY reads — is ignored at zero cost.
4. **RP demux** — the Core 0 loop in `oric_main` pops a pair: command word (`addr & 0xFFF` == `CMD_KEYPRESS` / `CMD_KEYRELEASE`), then the scancode word (`& 0x7F`). `kbdmap_isShift` / `kbdmap_isCtrl` are handled as modifier state; everything else goes through `kbdmap_StGsx2Ascii(scan_code, shift, ctrl)` — the Atari ST GSX scancode → Oric ASCII table in `kbdmap.c` — and then into `kbd_raw_key_down` / `kbd_raw_key_up`.
5. **Function keys are intercepted** in `kbd_raw_key_down` before reaching the Oric matrix. `kbdmap_initOric()` (called once from `oric_main`) patches the table so ST **F1** (`$3B`) → `0x13A`, ST **UNDO** (`$61`) → `0x144`, ST **HELP** (`$62`) → `0x145`, ST **HOME** (`$47`) → `0x148`, plus the four arrow keys and a small set of Ctrl combinations the Oric firmware needs. `0x13A` opens the menu (ESC closes it; both are consumed only while a menu screen is open), `0x144` raises NMI, `0x145` resets the Oric, `0x148` shows the timing readout. F2–F10 are not mapped and pass through to the Oric.

### Audio pipeline (AY-3-8912 → YM2149)

The emulated PSG's register writes are not synthesised on the RP; they are **replayed on the ST's real YM2149**. `oric_ayQueuePush(oric_via_queue, &oric_via_queue_head, value)` appends a 16-bit (register, value) pair into a 512-byte ring in the cartridge window and writes a `0xFFFF` terminator after it. Each VBL the m68k walks that ring from its own `AYBUFF_POS` cursor, and for every non-`$FFFF` entry writes the low byte pair to `$FFFF8800` (register select) and `$FFFF8802` (data), wrapping with `and.w #(AYBUFFER_SIZE - 1)`. It stops at the terminator. No timer, no interpolation — the Oric's own register traffic drives the ST's sound chip directly.

### Lower-border overscan

`.timerb_routine` counts down `.overscan_flag` from `LOW_BORDER_OVERSCAN_START` (190) and, when it hits zero, re-vectors Timer-B to `.timerb_overscan`, which does the classic sync-mode trick on `$FFFF820A` (write 0, wait, write 2) to remove the lower border so all 224 Oric lines fit on a 200-line PAL screen. The VBL handler re-arms the normal Timer-B vector every frame.

### Atari ST side (`target/atarist/`)
- `src/main.s` — the entire m68k side: cartridge header, boot, IRQ setup, VBL copy loop, Timer-B handler, overscan, reset. Lives at `$FA0000` (ROM4 cartridge region). `pre_auto` (CA_INIT bit 27, after GEMDOS init) checks the resolution, **copies the first `$1000` bytes of the cartridge image to `$50000`** (`SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET`, safely below screen memory) and jumps there — everything afterwards runs from ST RAM, not from the cart, which is why every internal reference is written as `SCREEN_A_BASE_ADDR - COPIED_CODE_OFFSET + (label - ROM4_ADDR)`.
- **Cart code must stay under `$0FFC`** — that is where the frame counter lives, and framebuffer A starts right after at `$1000`. This is the real size budget, not the 64 KB window.
- **No Atari RAM allocation**: the code uses no `.bss` and no heap. Its handful of variables live inside the copied code block itself, addressed through A6: `.vblank_flag` (a6+0), `.last_framecount` (a6+2), `.page_flag` (a6+4), `.overscan_flag` (a6+6), `.aybuff_pos` (a6+8, the slot `AYBUFF_POS` names). Those offsets are load-bearing — `AYBUFF_POS` is an `equ`, so a variable added or resized above it silently moves it.
- `src/inc/tos.s` — TOS/XBIOS equates and the `print` macro.
- **Dead code to be aware of**: the `check_keys` macro, `rom_function` and `CMD_BOOSTER` are defined but never invoked in the current path (leftovers from the microfirmware template's command-dispatch protocol). `check_commands` itself is also unused — it clobbers d6, which the VBL loop keeps as the 160-byte line stride — but `LISTENER_ADDR` is live: the loop polls it inline (into d0) for the reset handshake used by "Return to Booster". **d6 is live across the whole VBL loop**; anything added there must leave it alone. Note that `check_keys` also has its press/release branches inverted relative to the live `.timerb_routine` — don't use it as a reference.
- Built via `stcmd make release` (VASM/VLINK in Docker) → `dist/BOOT.BIN`.

### Shared cartridge region
The Atari ST sees the ROM4 window at `$FA0000`; the RP2040 backs it with the 32 KB `ROM_IN_RAM` region at `0x20030000` (`__rom_in_ram_start__`). This is the **single source of truth** for every cross-target data layout. Both sides derive their offsets symbolically — RP-side from `ATARI_ST_*` macros in `rp/src/reload/systems/oric/src/oric.h`, m68k side from the `equ`s at the top of `target/atarist/src/main.s`. **Never hard-code an address inside this region**; the two sides must be changed together.

| Offset | ST address | m68k symbol / RP macro | Size | Purpose |
| --- | --- | --- | --- | --- |
| `0x0000` | `$FA0000` | `ROM4_ADDR` | `$1000` | Cartridge image (`main.s`), ~1.3 KB used. Copied to `$50000` at boot and executed from there. |
| `0x05F8` | `$FA05F8` | `LISTENER_ADDR` / `ATARI_ST_LISTENER_OFFSET` | 4 B | RP→m68k command longword, polled once per VBL via `check_commands`. `REMOTE_RESET` (1) makes the ST wait `PRE_RESET_WAIT` (~1 s), invalidate memvalid and jump through `$4` — a cold reset, so TOS reinitialises palette, screen base, MFP and IKBD itself. Written by "Return to Booster" just before the RP reboots; cleared at startup because the region tail is never zeroed. **Exact-value longword:** store it half-swapped (`_oric_as_m68k_long`), since an m68k `move.l` is two word reads. Sits past the ~1.3 KB image but inside the `$1000` copied to ST RAM. |
| `0x0FFC` | `$FA0FFC` | `FRAMECOUNT_ADDR` / `ATARI_ST_FRAME_COUNTER_OFFSET` | 2 B (+2 B pad) | Frame counter. RP increments a `uint16_t` after each completed `oric_screen_update`; m68k `cmp.w`s it against its own saved copy once per VBL and re-blits on any change. Free to wrap — only inequality is tested. **16-bit deliberately:** an m68k `move.l` is two word reads, so a 32-bit value written natively by the RP would arrive with its halfwords swapped. The page (`$60000` vs `$70000`) is chosen by the m68k, which alternates locally. |
| `0x1000` | `$FA1000` | `FRAMEBUFFER_A_ADDR` / `ATARI_ST_FRAMEBUFFER_A_OFFSET` | 20160 B | Framebuffer A. The Oric screen in ST planar form: 224 lines × 90 bytes (240 px × 3 bitplanes). |
| `0x8000` | `$FA8000` | `FRAMEBUFFER_B_ADDR` / `ATARI_ST_FRAMEBUFFER_B_OFFSET` | 20160 B | Framebuffer B. Core 1 renders each frame into the buffer named by **bit 0 of the counter value it is about to publish**, so no extra shared field is needed. Backed by `ORIC_ROM_IN_RAM`, free since `oric_rom` moved to `ORIC_RAM`. |
| `0x5EC0` | `$FA5EC0` | `AYBUFFER_ADDR` / `ATARI_ST_VIA_QUEUE_OFFSET` | 512 B | AY register-write ring: 16-bit (register, value) entries, `$FFFF` terminator. RP pushes, m68k drains at VBL. |
| — | `$FAF000` | `ROMCMD_START_ADDR` | — | Not storage — a **read-only signalling window**. The m68k forwards key events by reading dummy bytes here; the RP's DMA IRQ records the address. Values read are meaningless and discarded (`tst.b`). |

`CENTERED_XPOS` (16) is an ST-screen offset, not a cartridge one — it centres the 240-pixel image on the 320-pixel line.

### RP2040 side (`rp/src/`)
- `main.c` — only checks the SELECT button, brings up `gconfig_init` (global config) then `aconfig_init` (per-app config), and hands off to `emul_start()`. If SELECT is held or config init fails it jumps to the **Booster** app via `reset_jump_to_booster()`. **Don't add features to `main.c`** — put them in `emul.c` or a new module.
- `emul.c` / `emul.h` — app entry point and the cart-bus capture layer. Copies the target firmware to `ROM_IN_RAM`, starts romemul with `emul_dma_irqHandlerLookup` as the DMA response callback, mounts the SD card at the configured folder (default `/oric`), then calls `oric_main()` and never returns. Also owns the 16-entry `emul_addrlog_*` ring that carries captured cart addresses to the emulator loop. **Note:** the IRQ handler reads `dma_hw->ch[2]` with a hard-coded channel index, while `romemul.c` allocates via `dma_claim_unused_channel()` — they agree today, but this is the thing that breaks first if DMA channel allocation changes.
- `reload/systems/oric/src/oric.c` — **the main application file.** Owns `state.oric`, the ROM loader (`rom.img` from the SD folder), the Core 0 emulation loop, `core1_main`, the keyboard demux and the F-key handling. `oric_main()` also sets the system clock (260 MHz), the flash baud divider and the core voltage before launching Core 1.
- `reload/systems/oric/src/oric.h` — header **and implementation** (`CHIPS_IMPL` style, defined by `oric.c`): `oric_init`, `oric_tick`, the memory map, `oric_screen_update`, `oric_show_msg`, and the `ATARI_ST_*` layout macros. The bulk of the emulator behaviour lives here, not in the `.c`.
- `reload/chips/*.h` — header-only chip models from Reload Emulator: `mos6502cpu.h` (the big one, ~8k lines of generated cycle-stepped 6502), `mos6522via.h`, `ay38910psg.h`, `mem.h`, `kbd.h`, `clk.h`, `beeper.h`, `wdc65C02cpu.h` (unused on Oric).
- `reload/devices/*.h` — `oric_td.h` (tape drive: plays `.tap` files straight from the card through a 256-byte ring; no `.wav` conversion any more) and `oric_microdisc.h` (WD1793 + Microdisc, ported from Oricutron under the permission in pete-gordon/oricutron#216 — see the file header for what differs from upstream and why). The upstream Disk II (`disk2_fdc.h` / `disk2_fdd.h` / `oric_fdc_rom.h`) is gone: it was an Apple II controller in the same address space that no Oric software targets.
- **The ST-visible cart window is a full, linear 64 KB.** The PIO preloads `__rom_in_ram_start__ >> 16` into the ISR and shifts the 16 bus address bits in beneath it, so a read at `$FAxxxx` resolves to `0x20030000 | xxxx`: offsets `$0000`–`$7FFF` are backed by `ROM_IN_RAM`, `$8000`–`$FFFF` by `ORIC_ROM_IN_RAM`. That is why framebuffer B at `$FA8000` and the `$FAF000` signalling window both work.
- **Microdisc wiring** (all in `oric.h`): `$0310–$0314` and `$0318` go to `microdisc_read/write`, the rest of `$0310–$031F` to the VIA (upstream's default branch). A `$314` write that changes ROMDIS or the EPROM bit calls `_oric_md_remap()`, which re-points the page table for `$C000–$FFFF` — BASIC ROM, or overlay RAM with the 8 KB EPROM over `$E000` — rather than testing the flags per access as Oricutron does. The WD1793 countdown ticks per cycle only while a delayed INTRQ/DRQ is pending; the Microdisc IRQ ORs into the VIA line at the existing `SET_IRQ`. Reset asserts ROMDIS **only when a disk is inserted** (D-17), so a disk-less power-on still lands in BASIC. `microdisc.rom` (exactly 8192 B) is loaded next to `rom.img`; absent → the controller does not exist and the registers read as before. The ROM picker skips that filename. Only one 6400-byte track of the image is ever in RAM (`diskimage_cachetrack` reads it from the card on a head move); writes dirty it, and it is flushed before replacement, on eject, and after ~2 s idle. **Two things that bit during bring-up and are now load-bearing:** the overlay RAM gets a non-zero power-on pattern (Sedoric's loader checksums it and loads only four sectors if it sums to zero — D-20), and the VIA drops its IRQ output when IER is cleared (Sedoric masks the VIA before every transfer; a stale T1 interrupt used to leak through and kill INTENA).
- `kbdmap.c` / `include/kbdmap.h` — Atari ST GSX scancode → Oric key translation: a `[128][2]` unshifted/shifted table plus `kbdmap_initOric()` fixups and the `kbdmap_isShift` / `kbdmap_isCtrl` predicates.
- `romemul.c` / `romemul.pio` — PIO + DMA cartridge ROM emulation (address in from `READ_ADDR_GPIO_BASE`, data out on the same pins, `ROM4_GPIO` as the bank select). Grants the DMA top bus priority. `init_romemul(request_cb, response_cb, copyFlashToRAM)` optionally installs an IRQ on either DMA channel — this app uses the response one.
- `gconfig.c` / `aconfig.c` — global vs per-app configuration in dedicated flash sectors, on top of `settings/` (a key-value store). This app's parameters are just `FOLDER` (default `/oric`) and `MODE`.
- `sdcard.c`, `hw_config.c` — FatFs over SPI/SDIO via the bundled `fatfs-sdk`.
- `include/memfunc.h` — `COPY_FIRMWARE_TO_RAM` (XIP-stream + DMA) and `ERASE_FIRMWARE_IN_RAM`.
- `include/reset.h`, `include/debug.h` — soft reset / jump-to-Booster, and the `DPRINTF` macro (compiled out unless `_DEBUG`).

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's flash and RAM are sliced into named regions, and code is responsible for not stomping on them:

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | App code |
| `ROM_TEMP` | `0x10100000` | 128 K | Scratch area for loaded ROMs |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (do not write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 128 K | Normal RAM |
| `ORIC_RAM` | `0x20020000` | 64 K | `.oric_ram` section — `oric_pat_lut`, `line_buff`, the overlay cells, the 16 KB `oric_rom[]` from `rom.img`, and the Microdisc's 16 KB overlay RAM, 8 KB EPROM and 6400 B track buffer. **64000 of 65536 B used** — nothing else fits here now |
| `ROM_IN_RAM` | `0x20030000` | 32 K | The cartridge window the Atari reads (`__rom_in_ram_start__`) |
| `ORIC_ROM_IN_RAM` | `0x20038000` | 32 K | Upper half of the ST-visible cart window (`$FA8000`–`$FAFFFF`); holds framebuffer B. The `.oric_rom_in_ram` section is empty — the region exists to reserve the address space |

Place data with `__attribute__((section(".oric_ram")))` / `".oric_rom_in_ram"`; the linker script exports `__oric_ram_start__`, `__rom_in_ram_start__` and `__oric_rom_in_ram_start__` for code that needs the base addresses (declared in `constants.h`).

The build assumes Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). **Core 1 is owned by `core1_main`** (the screen-conversion worker in `oric.c`); anything else that wants Core 1 has to replace it.

**Clock discrepancy to know about:** `constants.h` defines `RP2040_CLOCK_FREQ_KHZ` as 272000 and that value is what `GET_CURRENT_TIME_INTERVAL_MS` and the startup log use, but `oric_main()` actually calls `set_sys_clock_khz(260000, false)`. Voltage is `VREG_VOLTAGE_1_20`. If you touch either, change both.

### App identity
`CURRENT_APP_UUID_KEY` (set from the `APP_UUID_KEY` env var at CMake time, with a placeholder default) is the app's UUID4. It must match the `uuid` field in the built `desc/app.json` and is the key into `GLOBAL_LOOKUP_FLASH` that locates this app's config sector. Mismatch → the app jumps to Booster.

## Reference implementations — look here before inventing

Two sibling repos solve the same problem on the same silicon (RP2040 driving an
Atari ST over the cartridge bus) and are **the** first place to look before
writing anything new here. Port from them, with comments and attribution intact,
rather than reinventing:

- **`../md-framebuffer-template`** — the framebuffer/audio/input template.
  `rp/src/include/font8x8.h` + the `FB_FONT` descriptor (already ported here as
  `rp/src/include/font8x8.h` / `font.h`); `fb_chunked_asm.S` for the
  multiplication-based chunky→planar transpose
  (`(((q >> K) & 0x01010101) * 0x80402010) >> 28`) and the `ldmia`/`stmia` bulk
  copy; `fb_chunked.c` for the Core 0/Core 1 FIFO dispatch; per-file
  `#pragma GCC optimize("O3")` and `__not_in_flash_func` on hot paths;
  `cart_shared.h` for shared-region layout discipline.
- **`../md-gpu-demo`** — the same template with a worked demo suite. The
  `demo_*.c` files are the reference for per-frame optimisation technique:
  8.8 fixed point with sin/cos LUTs, power-of-two `&`-mask tiling, the SIO
  interpolator in texture-mapping mode, and the dual-core band split.

Note what does **not** transfer and why: md-oric has no chunked framebuffer
(Core 1 writes ST planar directly), and both cores are already committed —
Core 0 to the 6502, Core 1 to the screen conversion (D-04) — so the template's
Core 0/Core 1 band split cannot be copied as-is.

## Editing guardrails

- **Never modify** `pico-sdk/` or `fatfs-sdk/` — they are git submodules pinned to specific upstream revisions, and `rp/build.sh` re-pins them on every run. To change FatFs configuration, edit `rp/src/ff/ffconf.h` (project-owned override).
- Don't touch `main.c` for feature work — start in `emul.c` or `oric.c`.
- `rp/src/reload/` is vendored upstream code that has already been reorganised for this project. Keep changes there minimal and obvious; the `// SAFEGUARD START/END` comments mark the Atari-ST-specific additions grafted onto the original.
- Any change under `target/` requires regenerating `rp/src/include/target_firmware.h`.
- Match the existing C style (clang-format config in `.clang-format`, clang-tidy in `.clang-tidy` — both wired up via CMake when the binaries are on `PATH`). `cmake --build build --target clang-format` formats the tree.

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
