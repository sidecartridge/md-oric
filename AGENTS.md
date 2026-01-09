# AGENTS.md

This repository contains **md-doom**, a SidecarTridge Multi-device microfirmware
app template for Atari ST class machines. It follows the standard split between
RP2040 firmware (`rp/`) and target-computer firmware (`target/`).

See `README.md` for user-facing notes and the official SidecarTridge docs for
the latest programming guidance.

---

## Project layout (important)

- `rp/`
  RP2040-side firmware (hardware access, SD, UI/terminal, main loop).
- `target/atarist/`
  Target-computer firmware built with `stcmd`. Produces a binary that is
  embedded into the RP firmware.
- Submodules at repo root (do not vendor these):
  - `pico-sdk/`
  - `pico-extras/`
  - `fatfs-sdk/`
- `desc/`
  App metadata template (`app.json`) used by the root build script.
- `dist/`
  Build artifacts (UF2, JSON, md5sum).

**Key coupling:** `target/atarist/build.sh` generates `target_firmware.h` and
copies it into `rp/src/include/`. If you change anything under `target/`, you
must rebuild so the embedded header stays in sync.

---

## Entry points

- `rp/src/main.c` — firmware entry point.
- `rp/src/emul.c` — app logic and main loop.
- `target/atarist/` — target firmware sources and build scripts.

---

## Build prerequisites

- ARM embedded toolchain (`arm-none-eabi-*`).
- Python (for `target/atarist/firmware.py`).
- `stcmd` available on PATH (used by `target/atarist/build.sh`).
- Git submodules initialized.

---

## Build commands

### Full build (recommended)

```sh
./build.sh <board_type> <build_type> <app_uuid_key>
```

Example:

```sh
./build.sh pico_w debug 44444444-4444-4444-8444-444444444444
```

What it does:
- Copies `version.txt` into `rp/` and `target/`
- Builds target firmware and generates `rp/src/include/target_firmware.h`
- Builds RP firmware
- Stages artifacts into `dist/` and fills `desc/app.json`

Expected artifacts in `dist/`:
- `<APP_UUID>-<VERSION>.uf2`
- `<APP_UUID>.json`
- `rp.uf2.md5sum`

### RP-only build

```sh
cd rp
./build.sh <board_type> <build_type>
```

Notes:
- `rp/build.sh` **pins submodule tags** and deletes `rp/build/`.
- Both `rp/build.sh` and the root `build.sh` delete/replace `dist/`.

---

## Workflow rules for agents

- Do not discard or overwrite local changes without explicit user approval.
- Avoid running `./build.sh` or `rp/build.sh` unless asked; they delete build
  artifacts and pin submodule tags.
- You may compile targets directly (for verification) without invoking the
  build scripts.
- If you change protocol or target-side code, rebuild target firmware so
  `rp/src/include/target_firmware.h` is regenerated.
- Respect `.clang-format` and `.clang-tidy` in the repo root.
- Do not change submodule pins unless explicitly requested.

---

## “Done” checklist

- Targets compile successfully without using `./build.sh` or `rp/build.sh`.
- If target firmware changed, confirm `rp/src/include/target_firmware.h`
  was regenerated.
