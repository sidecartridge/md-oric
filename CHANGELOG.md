# Changelog

## v2.0.0 (unreleased)

### New features
- Floppy disk support (experimental): a Microdisc controller with a WD1793,
  ported from Oricutron with its author's permission. Standard `.dsk`
  (MFM_DISK) images are chosen from the menu and boot; reads and writes work,
  one drive. Needs the 8 KB Microdisc EPROM on the card as `microdisc.rom`.
- On-screen menu (F1): ROM selection, tape selection and eject, disk selection
  and eject, reset, status, help, return to Booster.
- Tapes play straight from `.tap` files, with a progress bar.
- Double-buffered display without the one-frame lag; screen conversion about
  30% faster.

### Changes
- **Breaking:** the F2–F10 tape-by-index convention is gone; tapes are chosen
  from the menu. ESC backs out of menus.
- **Breaking:** `.wav` tapes are no longer read or generated; `.tap` only.
- `rom.img` is written by the ROM menu rather than copied by hand.
- The upstream Disk II controller (an Apple II part) is removed.

### Fixed
- Keystrokes were lost and keys could stick down. The Atari ST now forwards
  IKBD bytes from the keyboard ACIA's own interrupt instead of polling them
  from a timer that stops during the vertical blank, the RP captures them
  through a DMA ring rather than a per-read interrupt that could overwrite
  itself, and a key is released with the same code it was pressed with.
- ESC now reaches the Oric; it was translated correctly but never registered
  on the emulated keyboard matrix.
- A reset rewinds the inserted tape instead of quietly forgetting it, so
  `CLOAD""` works after a reset. Reset from the menu is a power cycle, which
  also clears anything a disk DOS hooked into page 2.
- The 6522 model kept its IRQ line asserted after an enable was cleared with
  the flag pending; software that masks the VIA around disk transfers hung.
- RAM under the ROM now has a power-on pattern instead of zeros; Sedoric's
  loader depends on it.
- A cross-core race that could leave the loading message stuck (present in
  v1.0.1beta).

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
