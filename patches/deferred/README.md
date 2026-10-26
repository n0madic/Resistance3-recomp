# Deferred toolkit patches (not applied)

The full patches of the previous series, kept so that nothing found there is lost. `build_host.sh` and
`scripts/check_patches.sh` ignore this directory (they read `patches/domains.txt`); the live series is `patches/<domain>/`.

0001, 0002, 0003, 0004, 0005, 0007 and 0010 come from the Soulcalibur II HD port, rebased on ps3recomp
v0.16.0 (rev 749182ee; not re-checked against v0.17.0/v0.18.0, where parts are upstream: PS3_LOG_FIRST, the get-before-label
publish, the offline NetStartDialog answer, the thread priority at start). They apply **in this order** to the unpatched revision (0010 needs the context of 0002, so it does
not apply alone). 0040 fixes the test fixtures of that whole series and applies only on top of the old own patches
0020-0039 (superseded by the domains), it is kept for reference.

What the live series took from them: parts of 0002 (spurs/0001), 0004 (input/0001), 0005
(gcm-rsx/0005, 0006), 0007 (metal/0001, 0002), 0010 (module-abi/0001). What is left here, with the reason it is not
applied (Resistance 3 on macOS showed no need; re-check on the Windows/Linux hosts):

- 0001: (`PS3_LOG_FIRST` saturating log counters are in the toolkit since v0.17.0, #399); savedata root under `PS3_HDD0_ROOT` and `PS3_SAVEDATA_NODATA_OK`
  (upstream's `./gamedata/dev_hdd0/...` default is the same place); `PS3_DEV_FLASH` root list. (Its offline
  NetStartDialog answer is upstream since v0.18.0, #503.)
- 0002: SPU LS call hooks and `SPU_GETLOG`; WAIT_SIGNAL queue-ready wake, taskset generations and shared lock helpers
  (spu_workload.c).
- 0003: cellAudio diagnostics (`AUDIO_PEAK`, `AUDIO_WAV`), one pacing clock, ideal-block pacing on Windows (WASAPI).
- 0004: SDL gamepad on its own thread (hot-plug after start).
- 0005: display-list copy at the put store (`GCM_SNAPSHOT_CALLS`), GPU-kept NV3089 resolves, whole-surface readback
  cache, vertex-program analysis cache (performance; not measurable in Resistance 3's first level).
- 0007: FXAA/anisotropy of the live-draw path (the toolkit has `PS3_AA`, `PS3_ANISO`), present dump.
- 0010: optional LGPL-free ATRAC build, MinGW GCC fixes in ppu_loader.cpp.
(0012, a higher-priority new thread runs until it blocks, is upstream since v0.18.0 as the opt-in `PS3_PRIO_START=1`,
#416, and was dropped.)

Later own patches set aside (complete, applied on top of their domain at the time):

- metal-present-on-guest-cadence.patch (was metal/0003, 2026-10-23): shows frames one guest frame interval apart with
  presentDrawable:atTime: (frames held for exactly 4 refreshes at 120 Hz: 721 -> 801-858 of 900). The judder it was
  written for turned out to be shader translation stalls (the MSL translation cache, upstream #511), and with those gone no difference was visible;
  it costs half a vblank of latency. Revisit if 30 fps content judders on a ProMotion display.
