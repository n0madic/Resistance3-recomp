# Resistance 3 for macOS

A static recompilation of **Resistance 3** (PlayStation 3, EU release `BCES01118`) that runs natively on Apple Silicon
Macs with Metal. It is built on [ps3recomp](https://github.com/sp00nznet/ps3recomp): the game's PowerPC and Cell SPU
code is translated to C++ ahead of time and compiled into a native library, which a small host program loads together
with an HLE implementation of the PS3 system libraries (graphics, audio, input, file system, saves, trophies).

This repository contains **only the port's own sources**. It does not contain the game, and nothing derived from the
game is ever committed: you build the game module yourself, from your own copy of the disc.

## Status

- **Tested on macOS only** (Apple Silicon, Metal). In principle the game can run anywhere
  [ps3recomp](https://github.com/sp00nznet/ps3recomp) compiles, but no other platform has been tried: this
  repository's build and launch scripts target macOS, so elsewhere they would need adapting. The patch set is already
  organized by platform (`patches/domains.txt`).
- **Single-player campaign only.** Co-op and multiplayer are not supported.
- Keyboard + mouse and game controllers are supported; the game is playable with either.
- Most testing has been in the opening chapters (Haven). Expect rough edges further in.
- Only the EU release (`BCES01118`) has been tested.

## Requirements

**Hardware / OS**

- An Apple Silicon Mac (arm64) with Metal, running a current macOS. This is the only tested platform (see Status).
- About 25 GB of free disk space: ~17 GB for the game files, ~1.5 GB for build products, ~3 GB for the saves, the
  HDD install and the caches the game writes while it runs.

**Tools**

- Xcode Command Line Tools (`xcode-select --install`)
- `git`, `cmake`, `ninja`, `python3` (3.11 or newer), e.g. `brew install cmake ninja python`
- Internet access for the first build: it fetches the pinned toolkit and its dependencies from GitHub (SDL2, glslang,
  SPIRV-Tools, SPIRV-Cross), and the decrypt step downloads one file from RPCS3's repository (see below).

**Game files**

You need your own **decrypted dump of the disc's `PS3_GAME` folder** (EU, `BCES01118`). The port uses these parts of it:

```
PS3_GAME/PARAM.SFO
PS3_GAME/USRDIR/EBOOT.BIN
PS3_GAME/USRDIR/PACKED/GAME/*.PSARC
PS3_GAME/USRDIR/PACKED/levels/...
PS3_GAME/USRDIR/built/...             (movies and the rest of the game data)
```

The multiplayer levels are not needed. Voice packs: English (`GLOBAL_SOUND_US.PSARC`) and Russian
(`GLOBAL_SOUND_RU.PSARC`) are present in the tested copy; the port can run with either.

## Build

All commands run from the project root.

**1. Put the game files in place.** Copy the `PS3_GAME` folder into the project root, so that
`PS3_GAME/USRDIR/EBOOT.BIN` exists.

**2. Set up the Python environment** (used by the decrypt step and by the code lifter):

```bash
python3 -m venv .venv
```

```bash
.venv/bin/pip install pycryptodome capstone construct tabulate tqdm
```

**3. Decrypt the executable:**

```bash
.venv/bin/python -I tools/unself_app.py PS3_GAME/USRDIR/EBOOT.BIN -o PS3_GAME/USRDIR/EBOOT.elf
```

The tool keeps no keys of its own. It downloads RPCS3's `key_vault.cpp` at a pinned commit, checks its SHA-256 and reads
the executable's key from it. For an offline machine, download that file once and pass it with `--key-vault <file>`.

**4. Build the host and the game module:**

```bash
make
```

This runs two steps, which can also be run separately:

| Step | Command | What it does |
|---|---|---|
| Host | `make host` | Clones the pinned toolkit, applies the patches in `patches/`, builds the static dependencies and `dist/host/r3_host`. Takes a while the first time. Contains nothing from the game. |
| Module | `make module` | Lifts the decrypted `EBOOT.elf` and compiles `module/game.dylib` (about 6 minutes on the author's machine). |

## Run

```bash
make run
```

Options are passed as variables:

```bash
make run FPS=1 LOG=1 GAME_LANG=ru
```

| Variable | Effect |
|---|---|
| `FPS=1` | Print the presented frame rate in the terminal. |
| `LOG=1` | Write the host output to `logs/run-<time>.log` and `.log.err` instead of the terminal. |
| `GAME_LANG=ru` / `en` | Game language (voice pack and text). Without it the macOS language is used, falling back to English. |

Other environment variables are passed through to the host, for example `PS3RECOMP_FULLSCREEN=1 make run` starts in
full screen. `scripts/run_game.sh --help` shows the launcher's own options.

**First launch.**

1. The first boot, and the first boot after changing the language, shows **"the game will need to install data"**.
   Press **Space** (Cross). Nothing else needs to be done.
2. **Press START** appears roughly 45-50 seconds after launch. Press **Enter**.
3. On a fresh install the game may also show brightness and screen calibration screens; confirm them with Space.
4. Expect stutter at first: every shader is translated to Metal the first time it is used. The result is cached in
   `cache/msl`, so later runs are smooth.

## Controls

### Keyboard and mouse

| Input | Action | PS3 button |
|---|---|---|
| `W` `A` `S` `D` | Move | Left stick |
| Mouse | Look | Right stick |
| Left mouse button | Fire | R1 |
| Right mouse button | Aim | L1 |
| Middle mouse button or `V` | Melee | R3 |
| `Space` | Jump | Cross |
| `C`, `Ctrl` or `Backspace` | Crouch; back in menus | Circle |
| `E` or `R` | Reload, use | Square |
| `G` | Grenade | L2 |
| `F` | Secondary fire | R2 |
| `Shift` | Sprint | L3 |
| Mouse wheel or `Q` | Weapons: tap = previous weapon, hold = weapon wheel (pick with `WASD`) | Triangle |
| Arrow keys or `1` `2` `3` `4` | D-pad | D-pad |
| `Tab` | Select | Select |
| `Enter` / `Esc` | Select / back in menus; start / pause elsewhere | Cross / Circle, Start |

- The mouse is captured while the window is in front and no menu is open. `Cmd+Tab` releases it.
- `Cmd+Q` or **Quit** in the app menu asks the game to quit (it exits after 3 seconds if the game does not). The window's
  close button quits at once.
- The window can be resized; the aspect ratio is kept.

### Tuning

- `PAD_MOUSE_SENS=<value>` sets the mouse speed in degrees per point, e.g. `PAD_MOUSE_SENS=<value> make run`.
- The mouse is scaled by the field of view, so it covers the same part of the screen zoomed or not. `R3_ZOOM_SENS=0`
  turns that off.
- `PAD_KEYBOARD_LAYOUT=pad` switches to a pad-shaped layout: arrows, `I` `J` `K` `L` for the camera, `Z` `X` `A` `S`
  for the face buttons.

### Game controllers

Controllers that SDL2 recognizes as game controllers (PS4/PS5, Xbox, and most others) are read by the toolkit's pad
layer and can be plugged in at any time. The keyboard and mouse path is the one tested most.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `missing dist/host/r3_host` / `missing module/game.dylib` | The corresponding build step has not been run: `make`. |
| `missing PS3_GAME/USRDIR/EBOOT.elf` | Run the decrypt step (build step 3). |
| `r3_host is already running` | `make kill`. The host ignores a plain `kill`, so stale instances can survive a crash. |
| Textures or data stay broken from run to run | The game's disk cache (`cache/BCES01118`) can keep bad data after a data-path bug. `make clean-cache` removes it; the game rebuilds it. |
| Stutter on the first run | Normal: shaders are being translated. See "First launch". |
| Start over from scratch | Delete `gamedata/` (**this erases your saves and trophies**); the game installs its data again on the next start. |
| Something in the build is stale after `git pull` | Run `make` again. If the toolkit pin or the patches changed, the host is rebuilt, and the module then needs rebuilding too: host and module check each other's ABI. |

## How it works

- **Host** (`dist/host/r3_host`): the ps3recomp runtime plus this port's sources in `host/`, with every dependency linked
  statically. It is the same for everyone and contains nothing from the game.
- **Module** (`module/game.dylib`): the game's PPU and SPU code translated to C++ from *your* `EBOOT.elf` and compiled
  locally. The host loads it at start-up.
- **Patches** (`patches/`): changes to the pinned ps3recomp revision that this game needs, grouped by subject (SPURS,
  SPU runtime, RSX/GCM, input, file system, ...). Each one names the symptom it fixes. Fixes that apply to other games
  are proposed upstream.
- **Game data** (`PS3_GAME/`): served to the game as the disc (`/dev_bdvd/PS3_GAME`). What the game writes (HDD install,
  saves, trophies) goes to `gamedata/`.

```
host/        port sources: entry point, HLE overrides, input mapping
patches/     patches on top of the pinned ps3recomp revision
scripts/     build_host.sh, build_module.py, run_game.sh, SPU extraction, patch checks
tools/       unself_app.py (executable decryption)
Makefile     wrapper around the scripts above
```

Developers: [CLAUDE.md](CLAUDE.md) documents the architecture, the patch workflow, the test suites and the debugging
knobs in detail.

## Credits and disclaimer

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet: the recompilation toolkit and runtime this port is
  built on.
- [SDL2](https://libsdl.org), [glslang](https://github.com/KhronosGroup/glslang),
  [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) and
  [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross): window, input and shader translation.
- [RPCS3](https://github.com/RPCS3/rpcs3): `tools/unself_app.py` is a port of its executable decryption, and the key is
  read from its key vault.
- [envytools](https://github.com/envytools/envytools): the RSX memory-tiling address math.

Resistance 3 is a product of Insomniac Games and Sony Interactive Entertainment. This project is not affiliated with
or endorsed by either. It ships no game code or assets; you must own the game and supply your own copy.
