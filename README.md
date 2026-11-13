# Resistance 3 for macOS

A static recompilation of **Resistance 3** (PlayStation 3, EU release `BCES01118`) that runs natively on Apple Silicon
Macs with Metal. It is built on [ps3recomp](https://github.com/sp00nznet/ps3recomp): the game's PowerPC and Cell SPU
code is translated to C++ ahead of time and compiled into a native library, which a small host program loads together
with an HLE implementation of the PS3 system libraries (graphics, audio, input, file system, saves, trophies).

**You bring the game.** Nothing from the game, from Sony's firmware or from anyone's PS3 is in this repository or in the
release zip. On the first start the launcher takes *your* copy of the disc's `PS3_GAME` folder, translates its code and
compiles the result on *your* computer. The translated code never leaves it.

## Status

- **Tested on macOS only** (Apple Silicon, Metal). In principle the game can run anywhere
  [ps3recomp](https://github.com/sp00nznet/ps3recomp) compiles, but no other platform has been tried. The patch set is
  already organized by platform (`patches/domains.txt`).
- **Single-player campaign only.** Co-op and multiplayer are not supported.
- Keyboard + mouse and game controllers are supported; the game is playable with either.
- Most testing has been in the opening chapters (Haven). Expect rough edges further in.
- Only the EU release (`BCES01118`, version 01.00) is supported.

## What you need

| | |
|---|---|
| Computer | Apple Silicon Mac with Metal, a current macOS |
| Python | 3.10 or newer (`brew install python`, or python.org). Only the standard library is used |
| Compiler | the Xcode command line tools (`xcode-select --install`): Apple's clang builds the game module. A portable compiler is not an option: zig was tried and its linker cannot link the module (see [docs/architecture.md](docs/architecture.md)) |
| Disk | about 6 GB besides the game: the build, the saves, and the caches the game writes while it runs |
| Game | your own **decrypted dump of the disc's `PS3_GAME` folder** (EU, `BCES01118`); it stays where it is and is not changed |

The launcher uses these parts of the dump: `PARAM.SFO`, `USRDIR/EBOOT.BIN`, `USRDIR/PACKED/GAME/*.PSARC`,
`USRDIR/PACKED/levels/`, `USRDIR/built/` (movies and the rest of the game data). The multiplayer levels are not needed.
Voice packs: English (`GLOBAL_SOUND_US.PSARC`) and Russian (`GLOBAL_SOUND_RU.PSARC`) are present in the tested copy.

## Quick start

```sh
unzip r3-0.1.0-macos-arm64.zip
cd r3
xattr -dr com.apple.quarantine .      # only if macOS refuses to open the downloaded program

# first run: point it at the PS3_GAME folder of your dump (or the folder that holds it)
./r3.py --game "/path/to/PS3_GAME"

# every later run
./r3.py
```

The first run takes a few minutes (lifting the game's code, then compiling it into one library, `game.dylib`); later runs
start the game at once. Nothing is redone unless its inputs change.

1. **game**: decrypts the executable (`r3_host unself`, nothing is downloaded) and lays out the disc in the data directory
   as links to your files;
2. **lift**: translates the game's PPU code and its SPU programs to C/C++;
3. **compile**: builds that into `game.dylib` with all your cores;
4. **run**.

**First launch of the game.**

1. The first boot, and the first boot after changing the language, shows **"the game will need to install data"**.
   Press **Space** (Cross). Nothing else needs to be done.
2. **Press START** appears roughly 45-50 seconds after launch. Press **Enter**.
3. On a fresh install the game may also show brightness and screen calibration screens; confirm them with Space.
4. Expect stutter at first: every shader is translated to Metal the first time it is used. The result is cached in
   `cache/msl`, so later runs are smooth.

## Options

```
./r3.py [command] [--game PS3_GAME] [--fps] [--log] [--lang CODE] [--window WxH] [--fullscreen] [--sens DEG]
         [--unlock-fps]
```

| Option | Meaning |
|---|---|
| `--game PS3_GAME` | the PS3_GAME folder of your dump; remembered, so only the first run needs it |
| `--fps` | print the presented frame rate in the terminal |
| `--log` | write the host's output to `logs/run-<time>.log` and `.log.err` in the data directory |
| `--lang CODE` | game language (voice pack and text): `en`, `ru`, ... Without it the macOS language is used, falling back to English |
| `--window WxH` | initial window size in points, e.g. `1600x900`; the window can be resized, the aspect ratio is kept |
| `--fullscreen` | start in full screen |
| `--sens DEG` | mouse speed in degrees per point |
| `--unlock-fps` | up to 60 fps in play and cutscenes (the game's own cap is 30); the main menu stays at 30 |
| `--data DIR` | where everything derived from the game lives (default `./data` next to `r3.py`, or `R3_HOME`) |
| `--yes` | answer yes: confirm `reset --all` |

| Command | What it does |
|---|---|
| `run` (default) | build whatever is stale, then run |
| `build` | only build |
| `doctor` | check this machine and the installation, and say what is missing |
| `kill` | end a stuck host (it ignores a plain `kill`) |
| `clean-cache` | drop the game's disk cache (`--shaders` also the translated shaders) |
| `reset` | remove what is derived from the game (lift, module, disc layout); `--all` also the saves and caches |

`./r3.py --help` lists the rest. Other environment variables go to the host, for example `PAD_KEYBOARD_LAYOUT=pad`.

## Where things are

Everything derived from the game is in the data directory (`./data` unless you moved it):

| Path | What |
|---|---|
| `game.json` | which `PS3_GAME` folder was set up |
| `disc/PS3_GAME/` | the game as the host sees the disc: links to your files, plus the decrypted `EBOOT.elf` |
| `games/BCES01118-01.00/` | the decrypted executable, the lift, and `module/game.dylib` |
| `gamedata/dev_hdd0/` | **your saves and trophies**, and the game's HDD install |
| `cache/` | the game's disk cache (`BCES01118`) and the translated shaders (`msl`) |
| `controls.txt`, `logs/` | your key bindings, and the logs of `--log` runs |

Delete the data directory to start over (keep `gamedata/` if you want your saves). **Do not share the data directory or
the files in it:** they are derived from the game.

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

- `--sens <degrees per point>` sets the mouse speed (the environment variable `PAD_MOUSE_SENS` does the same).
- The mouse is scaled by the field of view, so it covers the same part of the screen zoomed or not. `R3_ZOOM_SENS=0`
  turns that off.
- The mouse follows the game's **Options > Controls**: Camera Speed and Invert-X / Invert-Y. `R3_GAME_SETTINGS=0` turns
  that off.
- `controls.txt` (written in the data directory, with the defaults, on the first run) remaps the keys and
  mouse buttons: one line per action, e.g. `jump = Space` or `fire = MouseLeft`. `R3_CONTROLS=<path>` names another file.
  Mistakes are reported in the log with their line number.
- The game's button prompts (menus, HUD, hints) show the key or mouse button bound to each pad button in
  `controls.txt`. They are drawn at start-up, so a change to the file shows after a restart. If you play with a game
  controller, `R3_KEY_GLYPHS=0` brings back the PlayStation icons.
- `PAD_KEYBOARD_LAYOUT=pad` switches to a pad-shaped layout: arrows, `I` `J` `K` `L` for the camera, `Z` `X` `A` `S`
  for the face buttons.

### Game controllers

Controllers that SDL2 recognizes as game controllers (PS4/PS5, Xbox, and most others) are read by the toolkit's pad
layer and can be plugged in at any time. The keyboard and mouse path is the one tested most.


## Troubleshooting

| Symptom | What to do |
|---|---|
| Something does not work | `./r3.py doctor` lists what is missing |
| `no suitable compiler` | `xcode-select --install` |
| `... is not a supported game version` | only `BCES01118` 01.00 (EU) is supported |
| `r3_host is already running` | `./r3.py kill` |
| Textures or data stay broken from run to run | `./r3.py clean-cache`; the game rebuilds its disk cache |
| Stutter on the first run | Normal: shaders are being translated. See "First launch" |
| Start over | `./r3.py reset`, or delete the data directory (`gamedata/` holds your saves and trophies) |
| After an update of the launcher | run `./r3.py`; a changed host rebuilds the module by itself (host and module check each other's ABI) |

## How it works

- **Host** (`host/r3_host`): the ps3recomp runtime plus this port's sources in `host/`, with every dependency linked
  statically. It is the same for everyone and contains nothing from the game. It also decrypts the executable
  (`r3_host unself`).
- **Module** (`game.dylib`): the game's PPU and SPU code translated to C++ from *your* executable and compiled locally.
  The host loads it at start-up.
- **Launcher** (`r3.py`, `launcher/`): Python standard library only. It sets the game up, lifts, compiles and starts the
  host; every step is skipped while its inputs are unchanged.
- **Patches** (`patches/`): changes to the pinned ps3recomp revision that this game needs, grouped by subject (SPURS,
  SPU runtime, RSX/GCM, input, file system, ...). Each one names the symptom it fixes. Fixes that apply to other games
  are proposed upstream.

[docs/architecture.md](docs/architecture.md) has the details, [docs/licensing.md](docs/licensing.md) the licenses, and
[CLAUDE.md](CLAUDE.md) the patch workflow, the test suites and the debugging knobs.

## Building from source

You need git, cmake, ninja, Python 3.10+ and the Xcode command line tools (`brew install cmake ninja python`) and internet
access for the first build, which fetches the pinned toolkit and its dependencies (SDL2, glslang, SPIRV-Tools, SPIRV-Cross).

```sh
scripts/build_host.sh     # clones the pinned toolkit, applies patches/, builds the static dependencies and the host -> dist/host
scripts/package.sh        # dist/r3-<version>-macos-<arch>.zip
./r3.py --game "/path/to/PS3_GAME"   # in a checkout the launcher builds and runs the host from dist/ itself
```

In a checkout `./r3.py` rebuilds the host with `scripts/build_host.sh` when `host/`, `patches/` or the build scripts are
newer than it (`--no-host-build` skips that). `make` wraps the same steps and uses the checkout itself as the data
directory (`R3_HOME`), so `make run` keeps its saves in `gamedata/`; `make test` runs the unit tests.

## License

The code in this repository is free software under the **GNU GPL, version 2 or (at your option) any later version**
([LICENSE-GPL-2.0.txt](LICENSE-GPL-2.0.txt)). The prebuilt host also contains Apache-2.0 licensed parts, so as a whole it
is distributed under **GPL version 3 or later** ([LICENSE](LICENSE)).

## Credits and disclaimer

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet: the recompilation toolkit and runtime this port is
  built on.
- [SDL2](https://libsdl.org), [glslang](https://github.com/KhronosGroup/glslang),
  [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) and
  [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross): window, input and shader translation.
- [RPCS3](https://github.com/RPCS3/rpcs3): `r3_host unself` ports its executable decryption, and the key table is from
  its key vault.
- [envytools](https://github.com/envytools/envytools): the RSX memory-tiling address math.
- The launcher follows the one of the Soulcalibur II HD port.

Resistance 3 is a product of Insomniac Games and Sony Interactive Entertainment. This project is not affiliated with
or endorsed by either. It ships no game code or assets; you must own the game and supply your own copy.
