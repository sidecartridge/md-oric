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
