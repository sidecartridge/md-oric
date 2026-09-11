# Changelog

## v2.0.0 (2026-09-11) - Menus, disks and a keyboard that keeps up

Everything is now chosen on screen. v1 needed a ROM renamed by hand and a
function-key convention to remember. Both are replaced by a menu. Floppy disk
support is new, and the input and display problems underneath are fixed.

### New features
- **On-screen menu**, opened with **F1**. It selects the BASIC ROM, inserts
  and ejects tapes and disks, resets the Oric, shows status and help, and
  returns to the Booster app without a power cycle.
- **Floppy disk support (experimental).** An Oric Microdisc with a WD1793,
  ported from Oricutron with its author's permission. Standard `.dsk`
  (`MFM_DISK`) images boot, and both reading and writing work on one drive.
  It needs the 8 KB Microdisc EPROM on the card as `microdisc.rom` or
  `microdis.rom`.
- **Tapes play straight from `.tap`** as they are read. There is no
  conversion step and nothing is written to your card. A progress bar along
  the bottom of the screen shows how far through the tape you are, so a
  stalled load is obvious.
- **ROM selection on first boot.** A single `.rom` on the card installs
  itself. If there are several you get a list to choose from, and if there
  are none the screen explains what to copy where, with a QR code to the
  setup guide.
- The **ESC** key now reaches the Oric.

### Changes
- **Breaking:** the F2-F10 tape-by-index convention is gone. Tapes are chosen
  from the menu, and F2-F10 now reach the Oric like any other key.
- **Breaking:** `.wav` tapes are no longer read or generated. `.tap` only, and
  a stale `.wav` left by an earlier version is deleted when the matching
  `.tap` is loaded.
- `rom.img` is written by the ROM menu rather than copied by hand.
- **RESET ORIC** from the menu is a power cycle: memory is cleared, so nothing
  a disk operating system left behind survives. The **HELP** key is still the
  soft reset that keeps your program.
- The upstream Disk II controller is removed. It was an Apple II part in the
  same address space; no Oric software targets it.
- Low resolution only, as before. The emulator returns to GEM with a message
  in any other resolution, and now also when no microSD card is present.

### Performance
- Screen conversion is about **30% cheaper per frame** (≈9900 µs → ≈6600 µs in
  text mode), using a multiply-based bit transpose in place of a per-pixel
  loop.
- The display is **double-buffered** and paced by the Atari's own
  blit-finished signal, so the one-frame lag and the tearing are gone. One
  frame of latency remains and is inherent, because the Atari must copy the
  screen before it can show it.

### Fixed
- **Lost keystrokes and stuck keys.** The Atari polled the keyboard from a
  timer that cannot fire during the vertical blank, so the ACIA overran and
  dropped bytes, releases included. The RP then captured what did arrive with
  a per-read interrupt that could overwrite itself. A key was also released
  with whatever code the modifiers produced at release time rather than the
  one it was pressed with. The keyboard is now driven by the ACIA's own
  interrupt into a DMA ring.
- **Black frames while typing.** Every MFP interrupt is level 6, so the
  keyboard handler could delay the cycle-exact lower-border trick and the
  shifter would lose lock for a frame.
- **ESC did nothing on the Oric.** It was translated correctly but never
  registered on the emulated keyboard matrix.
- **Tape loading after a reset.** A reset forgot the inserted tape instead of
  rewinding it, so `CLOAD""` did nothing while the menu still named the tape.
- **HIRES programs that switch mode and then overwrite the attribute** now
  work. The mode is latched when the CPU writes it, not only when the frame
  scan happens to see it.
- **The 6522 kept its IRQ line asserted** after an interrupt enable was
  cleared with the flag still pending, which hung anything that masks the VIA
  around a disk transfer.
- **RAM under the ROM starts with a power-on pattern** instead of zeros;
  Sedoric's loader checksums it and loads only part of itself if it sums to
  zero.
- **The cartridge firmware was copied at twice its length**, writing a
  kilobyte of unrelated flash over the shared signalling area.
- A cross-core race that could leave the loading message stuck on screen
  (present in v1.0.1beta).
- Ten font glyphs, including `g`/`q` and `,`/`.`, which were indistinguishable.

---

## v1.0.1beta (2026-01-14) - Bug fixes

### Fixed
- Handle missing CTRL/SHIFT key presses in Oric input.
- Clear memory on init to avoid stale state.
- Fix Atari ST framebuffer switching logic.

### Changes
- Docs: fix author attribution.
- Docs: fix typo.

### New features
- No new features.

---

## v1.0.0beta (2026-01-11) - Beta release
- First version

---
