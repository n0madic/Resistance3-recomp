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
  disk cache; delete it if a data-path bug may have poisoned it). The host keeps translated shaders in `cache/msl`
  (`PS3RECOMP_MSL_CACHE`, upstream #511; since #609 the translator is named by a hash of its sources): a first run
  stalls on every new shader, later runs do not. The files stay valid across host rebuilds; an edit to
  `rsx_shader_msl.cpp`/`.h` (their hash is the key) retires them.

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
- The module's compile cache key includes the SDK ABI (a hash of the toolkit headers and lifters), so a header change
  recompiles every unit.

### Tests (toolkit)

Toolkit tests build with `-DPS3RECOMP_BUILD_TESTS=ON` and run through ctest. Configure with the same flags the host
build uses, or the Homebrew SPIRV-Cross is picked up and `rsx_shader_msl.cpp` does not compile:

```bash
cmake -S build/ps3recomp -B <scratch>/build-tests -G Ninja -DPS3RECOMP_BUILD_TESTS=ON \
      -DCMAKE_PREFIX_PATH="$PWD/build/deps/prefix" -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \
      -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
cmake --build <scratch>/build-tests -- -k 0     # keep going past a target that does not link
ctest --test-dir <scratch>/build-tests -R gcm_put_jump --output-on-failure   # one test; drop -R for all 40
```

Most toolkit tests are not in ctest: `python3 build/ps3recomp/tools/test_<name>.py` builds and runs one (144 of them with
the patches; not `-I`: seven import sibling tool modules, which isolated mode cannot find, and fail for that reason alone).
Some runtime unit tests (e.g. `runtime/spu/tests/test_spu_thread_ls_dma.c`) are built directly with clang from the
toolkit dir against the runtime sources they need: the "run:" steps of `.github/workflows/macos.yml` give the line
(37 of them).
The Metal ones that translate guest programs
(`libs/video/tests/test_shadow_compare_metal.m`) need the translator: compile `rsx_shader_msl.cpp` with
`-DPS3RECOMP_HAVE_MSL_TRANSLATION=1 -DPS3RECOMP_MSL_TRANSLATOR_ID='"<any 23 chars>"'` and `-I build/deps/prefix/include`
(+ `/spirv_cross`), and link glslang, SPIRV, SPIRV-Tools(-opt) and the spirv-cross static libraries from
`build/deps/prefix/lib`; without it the stub reports "translation was not built" and 32 of 37 checks fail (with it,
133/133 pass). On the pinned v0.19.0 with the patches on, 38 of the 40 ctest tests, all 144 scripts and the 37 workflow
tests pass (`test_metal_frame_rate` needs a usable display); `sys_fs` and `ppu_thread_join` do not link on macOS, on
plain v0.19.0 either (`_cellGame_get_hdd_title_id`; Windows thread calls), so any other new failure is ours. A patch that moves a symbol or
changes a code path can break a fixture's link line or stub (the loader fixtures stub `cellGcm_on_put_store`): fix the
fixture in the same patch.

## Toolkit patches (`patches/`)

All runtime/lifter changes live as patches on top of the pinned toolkit revision (`TOOLKIT_REV` in
`scripts/build_host.sh`; upstream is `github.com/sp00nznet/ps3recomp` and has priority over everything here).

- **Layout.** `patches/<domain>/NNNN-*.patch`, one directory per subject area (module-abi, spurs, spu-runtime,
  savedata-trophy, input, lv2, fs, gcm-rsx), listed in `patches/domains.txt` (second column: platforms the domain
  is for). Patches inside a domain go in name order and may depend on each other; **domains never depend on each other**:
  each applies to the base on its own, and any order of them gives the same tree.
  `patches/upstream/*.patch` holds open upstream PRs imported as they are (header + the PR's diff), applied first, in
  name order. **The base is the pin plus those PRs**: our own patches go last and are rebased onto an imported PR
  (as upstream will have to be the starting point anyway once it merges, or after a pin move). An imported PR's header
  still names the Resistance 3 symptom; a perf PR needs a measurement here.
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
- **Making a patch.** In a scratch clone of the pinned revision with the `patches/upstream` PRs applied, put the change
  on top of the *domain's* earlier patches only (not of other domains), commit it, and write
  `header, "---", blank, git diff -U1` (narrow context keeps domains independent and survives upstream churn). Files here have mixed CRLF/LF lines: edit them byte-safe (a text-mode
  Python rewrite normalises the endings and the patch then carries hundreds of spurious lines; `check_patches.sh` does
  not catch that, `git diff --ignore-cr-at-eol --stat` does). New behaviour gets a test inside the patch.

## Architecture notes (cross-file)

- **PPU**: `ppu_loader.py` + `ppu_lifter.py` lift EBOOT up to `CODE_END` (`build_module.py`; `.rodata` follows in the
  same segment). Firmware imports resolve by NID to HLE handlers in the toolkit's `libs/` (cellGcm, cellFs, cellPad,
  cellSpurs, cellSaveData, sceNpTrophy, …); port-specific overrides are in `host/r3_hle.cpp`.
- **SPU**: three kinds of code, all inside the EBOOT (none in PSARC data):
  - SPU ELFs (`extract_spu_images.py`);
  - SPURS policy modules `pm_wws` / `pm_ig`, at LS 0xA00;
  - WWS and Insomniac job modules. Insomniac ones are linked at LS 0x100, except the buffer modules (a second header
    layout) that the dispatcher loads into fixed LS buffers (0x6600, 0x20680, 0x24D00, ...) and the dispatcher itself
    (0x3F580); the load addresses of some are a table in `extract_spu_raw.py` (`IG_BUFFER_MODULES`, from census runs),
    every other second-layout header in the EBOOT is lifted position-independent (`--pic`, verified wherever it loads).
    Functions a job reaches only through a pointer are invisible to the lifter: `IG_EXTRA_ENTRIES` lists them per module
    (from a census of lookup misses), passed as `--extra-funcs`. Lifting with `--return-entries` hangs the boot.
    The WWS job modules get theirs from `WWS_JOB_EXTRA_ENTRIES` in `build_module.py`, keyed by module (`spu-runtime/0010`): they all load at LS 0x4000, so a bare address would also split every other module that spans it. A census keyed by the resident
    span an interpreter entry lands in (its load's source EA and the offset in it) is exact. Keyed by the module
    header below the PC, it misplaces the data-segment modules, which load over each other. After the 2026-10-25
    census of a long Haven firefight, what still runs interpreted there is `pm_wws`'s stores into its own code, where
    the SMC microstep hands each one to the interpreter.

  `scripts/extract_spu_raw.py` wraps the non-ELF code as ELFs for lifting. `build_module.py` generates
  `spu/igjob_regions.c`, which registers each Insomniac module's span as resident code (buffer modules as verified
  regions with their bytes): they overwrite `pm_ig` in LS, so the dispatch must follow `resident_code`. A DMA from
  elsewhere into a span only takes over the bytes it wrote (`spu-runtime/0007`). Code that is not lifted runs in the
  SPU interpreter. The ~220 images need larger runtime tables than the toolkit's defaults: `host/CMakeLists.txt` defines
  `SPU_WORKLOAD_MAX` (past it the SPURS task ELFs were dropped and the boot hung) and `SPU_OVL_SRC_MAX`
  (`spu-runtime/0009`; past it a module stays in the interpreter, the log says `[r3] ... not registered`).
- **RSX/GCM**: the FIFO walker runs on the present thread. `cellGcm_on_put_store` takes JUMPs at an idle get
  immediately (the game injects command blocks at get and reuses their memory). The draw engine goes to Metal.
  The game reads its Z24S8 depth buffers as A8R8G8B8 several times a frame (one of them while it is still the
  bound zeta), and its Z16 shadow maps as DEPTH16; the backend converts both on the GPU (`depth_as_color`,
  upstream #499 since v0.18.0; `PS3RECOMP_ENG_DEPTH_VIA_GUEST=1` brings back the store through guest memory, which cost
  ~3 fps).
  Lamp shadows need upstream #501 and #499 (ours, in v0.18.0): the light shaders read the DEPTH16 maps through
  crossbar 0xA900 (depth formats
  present depth on every lane), and the engine's 64-entry surface/zeta tables evict the least recently used entry
  (before, the maps Home renders at load got no depth target). The compare order is PR #424's (depth FUNC ref).
  Close to a lamp the game switches its map to a swizzled power-of-two Z16 surface (zeta pitch 64), read as swizzled
  DEPTH16 (`gcm-rsx/0016`; before, the lamp's light vanished near it).
  The menu atlases (DXT) share local memory with the level's post-processing targets and the engine's target table
  outlives the level: a block-compressed unit never reads a target (upstream #508; before, the menu
  after leaving a mission was noise on a flat background).
  An NV3089 copy that lands over a target it does not update (another pitch) makes guest memory the newer copy until
  the target is drawn again (`gcm-rsx/0018`; before, the hurt screen showed a stretched bloom target over black).
  A vertex attribute at frequency 1 with its divider-operation bit set (modulo) is element 0 for every vertex: the
  tracer trails pass their start point and colours that way (upstream #547; before, trails drew wedges across the
  screen).
  Under load the game drops its scene from 704 to 640 rows at the same addresses; the engine keeps the 704-row target
  and a texture descriptor smaller than a target reads a copy of its top-left (`gcm-rsx/0020`; recreating the target
  instead gave a black frame at the onset of a heavy hit, binding it whole a yellow band at the bottom).
  Tile 4 is bound over main memory (IO 0x30000, pitch 8192, bank 2). While aiming, NV3089 downsamples the scene into
  it for an SPU depth-of-field job that reads and writes the tiled layout. Every RSX access to a main-memory tile
  goes through the tiling (upstream #569, ours, in v0.19.0; the NV0039 copy is `gcm-rsx/0021`). The address math is `rsx_tiling.h`, from envytools'
  `tile_translate_addr` (MIT; upstream will not take GPL ports such as RPCS3's).
  `rsx_tiling.c` holds the one tile table and the range copies used by the 2D engines, NV0039, and the draw engine's
  surfaces, textures and fetches. Before that, the job's output showed as bands and the blur never appeared. Only
  the NV3089 and texture paths are reached in the scenes checked.
  Explosion smoke (rockets, hybrids blowing up) samples 256x256x16 DXT5 volumes in main memory, stored in NVIDIA's
  VTC order; the pin's 3D path took uncompressed formats only, and the first 64 KB read as a 2D image drew dark
  squares (upstream #585, ours, in v0.19.0).
  A frame of the Haven fight binds ~1700 distinct textures; the engine's texture cache held 1024 and re-created
  ~700 a frame (22 ms of the RSX thread's CPU). `host/CMakeLists.txt` sets it to 2048 (`gcm-rsx/0023`).
  The fight records ~7800 draws a frame, and the RSX thread is what the game waits on (the main thread polls a GCM
  label in `usleep(50)`, `func_0073439C`). The engine keeps each draw's converted vertices and rebuilt indices in
  GPU-read chunks keyed by a hash of the index bytes, layout, attribute state and vertex bytes (`gcm-rsx/0024`;
  99.5% hits there; `PS3RECOMP_ENG_VCACHE=0` turns it off, `PS3RECOMP_ENG_VCACHE_STATS=1` prints the hit rate). An
  attribute's current value is keyed only where it reaches a vertex (no array): keying it always cost 15-20% misses.
  Past the Metal backend's record cap the frame so far is submitted and waited for, as for a full staging arena
  (`gcm-rsx/0025`; it used to drop the rest: at 8192 the fight lost 117-195 records every few frames); the cap is
  16384 from `host/CMakeLists.txt`, so the fight never pays that mid-frame wait. Next on that thread: the 8 KB VS constant block copied for every draw (`eng_bind_vs_constants`
  -> `eng_stage_copy`, ~10%), texture content hashing, FP structural hashing.
  The game times its GPU frame with marker reports (IO 0x20B60 start, 0x20B40 end; `func_0075A38C` keeps the time at
  0x12B5238+0xE1C, `func_003DCA40` compares it with its frame budget and sets the quality bits at 0x11DDFB4) and
  drops decor (cloth under roofs, bushes, saplings) above 0.84-0.90 of the budget. The walker stamped reports with
  host time, so the game read our draw conversion as GPU load; `PS3RECOMP_GCM_REPORT_BUSY_SCALE=0` (host default,
  `gcm-rsx/0026`) leaves the walker's drain time out of the stamps. Its ~2360 occlusion queries a frame are read only by
  conditional rendering (method 0x1E98), which the toolkit ignores: everything is drawn.
  `host/main.cpp` sets env defaults (`set_default_env`: `GCM_SUBCH1_2D=1`, `PAD_KEYBOARD_LAYOUT=fps`,
  `PS3_HDD0_ROOT`, `SPU_INTERP_UNLIFTED=1`, …) and `ppu_hle_inject_base=0x5F000000` (the game's 256 MB mmapper heap lands at 0x20000000); the runtime looks most switches up once per name (`ps3_env_once`, #515), so a variable set after start-up is not seen.
- **Language**: `PS3_LANG` (`ru`, `en` or a number). Otherwise the host locale is used, falling back to English.
  Voice packs present: English and Russian. Switching language makes the game ask once to install data again.

## Running/debugging knobs

- **Keyboard and mouse** (the keyboard fallback is upstream, #612; cellPad's fps layout is `patches/input/0004`; the game never reads cellMouse. In play the
  mouse turns the hero's orientation matrix directly, `host/r3_input.cpp` via `patches/input/0006`; in menus and
  cutscenes it moves the right stick, capped at the game's top stick rate):
  - WASD walk, mouse look (cursor captured while the window is in front and no menu is up, `patches/input/0007`;
    Cmd+Tab frees it too). The window's close button quits at once (`host/main.cpp`); Cmd+Q and the app menu's Quit item (#613) ask the game to quit
    (upstream `ps3_host_quit`, #544: REQUEST_EXITGAME, then exit after `PS3_QUIT_GRACE_MS`, 3 s); the window resizes
    keeping the aspect ratio and `PS3RECOMP_FULLSCREEN=1` starts it full screen (#614);
  - left button = R1 (fire), right = L1 (aim), middle or V = R3 (melee), wheel or Q = triangle (weapons);
  - Space = cross (jump), C/Ctrl/Backspace = circle (crouch, back), E/R = square (reload, use);
  - G/F = L2/R2 (grenade, secondary fire), Shift = L3 (sprint);
  - arrows or 1/2/3/4 = d-pad, Tab = select;
  - Enter/Esc = cross/circle (select, back) in menus and start elsewhere (Press START, pause, skip); the host tells
    menus apart by the game's own flags (`host/r3_input.cpp`, `patches/input/0005`);
  - `PAD_MOUSE_SENS` = degrees per point, `PAD_MOUSE_AIM_SCALE` = its factor while aiming (defaults in
    `host/main.cpp`); `PAD_KEYBOARD_LAYOUT=pad` gives the pad-shaped layout (arrows, I/J/K/L camera, Z/X/A/S face
    buttons).
- **Scripted input**: `PAD_FILE=<file>`, where the file holds `mask reports [lx ly rx ry]` (a report is a pad read that
  returned data, #570), e.g. `echo "0x4000 8" > f` presses cross. Add `PAD_NO_KEYBOARD=1` to a scripted run: else
  bringing its window to the front captures the mouse, which turns the camera.
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
