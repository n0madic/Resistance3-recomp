# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Static recompilation of Resistance 3 (PS3, EU BCES01118) for macOS (arm64, Metal) with
[ps3recomp](https://github.com/sp00nznet/ps3recomp). Single-player only. The project root is a git repository that tracks
only the port's own sources (`.gitignore` is a whitelist: `host/`, `patches/`, `scripts/`, `tools/`, `CLAUDE.md`); game
data, the lifted module, build products and logs must never be committed. `build/ps3recomp` is a separate git clone
(the pinned toolkit).

## Layout and the two build products

- **Host** `dist/host/r3_host`: the ps3recomp runtime + this port's sources in `host/` (`main.cpp`, `stubs.cpp`,
  `r3_hle.cpp`, `spurs_lfqueue.cpp`), all deps linked statically. Contains nothing derived from the game. It also
  writes `dist/host/sdk/` (headers, lifter tools, `ABI` string) that the module is built against.
- **Module** `module/game.dylib`: the lifted PPU + SPU code of the user's `EBOOT.elf`, loaded by the host at start.
  Host and module check the ABI string; a toolkit change that alters the ABI needs a module rebuild.
- **Game data** `PS3_GAME/` (decrypted; served as `/dev_bdvd/PS3_GAME`). The ISO is gone: `PS3_GAME/` is the only
  copy. `PACKED/GAME/*.PSARC` is uppercase locally; the host FS is case-insensitive.
- Runtime state written by the game: `gamedata/dev_hdd0/` (HDD install, saves, trophies), `cache/BCES01118` (FIOS
  disk cache; delete it if a data-path bug may have poisoned it).

## Commands

```bash
scripts/build_host.sh                 # full host build: clone/pin toolkit, apply patches, static deps, host -> dist/host
cmake --build build/host && cp build/host/r3_host dist/host/   # after host-only edits (host/*.cpp)
.venv/bin/python scripts/build_module.py              # lift + compile module/game.dylib (~6 min)
.venv/bin/python scripts/build_module.py --skip-lift  # recompile only, reuse the lift in module/
scripts/run_game.sh [--fps] [--log] [--lang ru|en]    # run; --log writes logs/run-<time>.log{,.err}
.venv/bin/python -I tools/unself_app.py PS3_GAME/USRDIR/EBOOT.BIN -o PS3_GAME/USRDIR/EBOOT.elf   # decrypt
# unself_app.py keeps no key: it downloads RPCS3's key_vault.cpp at a pinned commit (SHA-256 checked) and reads the
# APP keys from it; --key-vault <file> uses a local copy instead (offline).
```

- `r3_host` ignores SIGTERM. Always `pkill -9 -x r3_host` before a run (stale instances survive and skew results);
  for timed runs use `timeout -k 3 N`. Use `pgrep -x` (not `-f`, which matches `timeout`).
- The module's compile cache key ignores headers: after toolkit header changes delete `module/obj`.

### Tests (toolkit)

Toolkit tests build with `-DPS3RECOMP_BUILD_TESTS=ON` and run through ctest. Configure with the same flags the host
build uses, or the Homebrew SPIRV-Cross is picked up and `rsx_shader_msl.cpp` does not compile:

```bash
cmake -S build/ps3recomp -B <scratch>/build-tests -G Ninja -DPS3RECOMP_BUILD_TESTS=ON \
      -DCMAKE_PREFIX_PATH="$PWD/build/deps/prefix" -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \
      -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
cmake --build <scratch>/build-tests -- -k 0     # keep going past a target that does not link
ctest --test-dir <scratch>/build-tests -R gcm_put_jump --output-on-failure   # one test; drop -R for all 16
```

Most toolkit tests are not in ctest: `python3 build/ps3recomp/tools/test_<name>.py` builds and runs one (76 of them on
v0.17.0; not `-I`: seven import sibling tool modules, which isolated mode cannot find, and fail for that reason alone).
Some runtime unit tests (e.g. `runtime/spu/tests/test_spu_thread_ls_dma.c`) are built directly with clang from the
toolkit dir against the runtime sources they need. On the pinned v0.17.0 all 16 ctest tests and all 76 scripts pass with
the patches on (`test_metal_frame_rate` needs a usable display), so a new failure is ours. A patch that moves a symbol or
changes a code path can break a fixture's link line or stub (the loader fixtures stub `cellGcm_on_put_store`): fix the
fixture in the same patch.

## Toolkit patches (`patches/`)

All runtime/lifter changes live as patches on top of the pinned toolkit revision (`TOOLKIT_REV` in
`scripts/build_host.sh`; upstream is `github.com/sp00nznet/ps3recomp` and has priority over everything here).

- **Layout.** `patches/<domain>/NNNN-*.patch`, one directory per subject area (module-abi, spurs, spu-runtime,
  savedata-trophy, input, lv2, fs, gcm-rsx, metal), listed in `patches/domains.txt` (second column: platforms the domain
  is for). Patches inside a domain go in name order and may depend on each other; **domains never depend on each other**:
  each applies to the pinned revision on its own, and any order of them gives the same tree.
  `patches/upstream/*.patch` holds open upstream PRs imported as they are (header + the PR's diff), applied first.
  `patches/deferred/` keeps the full patches that were left out (not applied; see its README) so nothing is lost.
- **What may be in a patch.** Only what this game needs: the header names the symptom in Resistance 3 and the test (or
  scripted run) that shows it. Nothing is carried over because another port has it. If upstream already offers the
  feature (env knob, option), use that from `host/main.cpp` instead of patching. Every header has an `Upstream:` line
  (not proposed yet / PR #N / why not); generic fixes should be sent upstream, and the patch goes when the pin has it.
- **`scripts/check_patches.sh [rev]`** classifies every patch against a toolkit revision (`INCLUDED` = upstream has it,
  drop it; `APPLIES`; `CONFLICT` = rebase) and proves the independence above. Run it before and after moving the pin
  (`git fetch --tags` in `build/ps3recomp` first), then `scripts/build_host.sh`, rebuild the module, run the tests above
  and a scripted run (boot, new game, walk; look at the frames: a numeric diff of frames missed a real effect once).
- `build_host.sh` resets `build/ps3recomp` to a clean checkout and re-applies everything whenever the revision or any
  patch changes (stamp in `.git/r3-patches`). **Uncommitted edits in `build/ps3recomp` are wiped by that**: turn them
  into a patch first.
- **Making a patch.** In a scratch clone of the pinned revision, put the change on top of the *domain's* earlier patches
  only (not of other domains), commit it, and write `header, "---", blank, git diff -U1` (narrow context keeps domains
  independent and survives upstream churn). Files here have mixed CRLF/LF lines: edit them byte-safe (a text-mode
  Python rewrite normalises the endings and the patch then carries hundreds of spurious lines; `check_patches.sh` does
  not catch that, `git diff --ignore-cr-at-eol --stat` does). New behaviour gets a test inside the patch.

## Architecture notes (cross-file)

- **PPU**: `ppu_loader.py` + `ppu_lifter.py` lift EBOOT up to `CODE_END` (`build_module.py`; `.rodata` follows in the
  same segment). Firmware imports resolve by NID to HLE handlers in the toolkit's `libs/` (cellGcm, cellFs, cellPad,
  cellSpurs, cellSaveData, sceNpTrophy, …); port-specific overrides are in `host/r3_hle.cpp`.
- **SPU**: three kinds of code, all inside the EBOOT (none in PSARC data):
  - SPU ELFs (`extract_spu_images.py`);
  - SPURS policy modules `pm_wws` / `pm_ig`, at LS 0xA00;
  - WWS and Insomniac job modules (Insomniac ones are linked at LS 0x100).

  `scripts/extract_spu_raw.py` wraps the non-ELF code as ELFs for lifting. `build_module.py` generates
  `spu/igjob_regions.c`, which registers each Insomniac module's span as resident code: they overwrite `pm_ig` in LS,
  so the dispatch must follow `resident_code`. Code that is not lifted runs in the SPU interpreter.
- **RSX/GCM**: the FIFO walker runs on the present thread. `cellGcm_on_put_store` takes JUMPs at an idle get
  immediately (the game injects command blocks at get and reuses their memory). The draw engine goes to Metal.
  `host/main.cpp` sets env defaults (`set_default_env`: `GCM_SUBCH1_2D=1`, `PAD_KEYBOARD_LAYOUT=fps`,
  `PS3_HDD0_ROOT`, `SPU_INTERP_UNLIFTED=1`, …) and `ppu_hle_inject_base=0x5F000000` (the game's 256 MB mmapper heap lands at 0x20000000).
- **Language**: `PS3_LANG` (`ru`, `en` or a number). Otherwise the host locale is used, falling back to English.
  Voice packs present: English and Russian. Switching language makes the game ask once to install data again.

## Running/debugging knobs

- **Keyboard and mouse** (cellPad's fps layout, `patches/input/0004`; the game never reads cellMouse. In play the
  mouse turns the hero's orientation matrix directly, `host/r3_input.cpp` via `patches/input/0006`; in menus and
  cutscenes it moves the right stick, capped at the game's top stick rate):
  - WASD walk, mouse look (cursor captured while the window is in front; Cmd+Tab frees it);
  - left button = R1 (fire), right = L1 (aim), middle or V = R3 (melee), wheel or Q = triangle (weapons);
  - Space = cross (jump), C/Ctrl/Backspace = circle (crouch, back), E/R = square (reload, use);
  - G/F = L2/R2 (grenade, secondary fire), Shift = L3 (sprint);
  - arrows or 1/2/3/4 = d-pad, Tab = select;
  - Enter/Esc = cross/circle (select, back) in menus and start elsewhere (Press START, pause, skip); the host tells
    menus apart by the game's own flags (`host/r3_input.cpp`, `patches/input/0005`);
  - `PAD_MOUSE_SENS` = degrees per point, `PAD_MOUSE_AIM_SCALE` = its factor while aiming (defaults in
    `host/main.cpp`); `PAD_KEYBOARD_LAYOUT=pad` gives the pad-shaped layout (arrows, I/J/K/L camera, Z/X/A/S face
    buttons).
- **Scripted input**: `PAD_FILE=<file>`, where the file holds `mask polls [lx ly rx ry]`, e.g. `echo "0x4000 8" > f`
  presses cross.
- **Screenshots**: `RSX_ENGINE_SNAP_DIR=<dir> RSX_ENGINE_SNAP_EVERY_S=5`; per-pass dumps: `RSX_ENGINE_PASS_DUMP`.
- **Logs**: `PS3_VERBOSE=1` brings back the toolkit's per-wait lines (`[WAIT] ...`); the host turns them off because
  the toolkit enables them whenever stderr is redirected. Other traces have their own variables (`PS3_FSLOG`,
  `SPU_CH_BLOCK_TRACE`, `SPURS_PM_TRACE`).
- **Store tracing**:
  - `PPU_WWATCH=<addr>` logs PPU stores to a guest address, with the guest function;
  - `SPU_WATCHEA=<hex>` logs SPU DMA to an EA.
- **Native debugging**:
  - `sample <pid> 3 -file f` shows guest function names;
  - in lldb, guest memory is at `vm_base` (big-endian words).
- **Boot timeline**:
  - with an existing install, Press START appears about 47 s after launch;
  - the first boot after an install or a language change shows the install prompt (press cross).
