# Deferred toolkit patches (not applied)

The full patches of the previous series, kept so that nothing found there is lost. `build_host.sh` and
`scripts/check_patches.sh` ignore this directory (they read `patches/domains.txt`); the live series is `patches/<domain>/`.

0001, 0002, 0003, 0004, 0005, 0007, 0010 and 0012 come from the Soulcalibur II HD port, rebased on ps3recomp
v0.16.0 (rev 749182ee; not re-checked against v0.17.0, where parts are upstream: PS3_LOG_FIRST, the get-before-label publish). They apply **in this order** to the unpatched revision (0010 needs the context of 0002, so it does
not apply alone). 0040 fixes the test fixtures of that whole series and applies only on top of the old own patches
0020-0039 (superseded by the domains), it is kept for reference.

What the live series took from them: parts of 0002 (spurs/0001), 0004 (input/0001), 0005
(gcm-rsx/0005, 0006), 0007 (metal/0001, 0002), 0010 (module-abi/0001). What is left here, with the reason it is not
applied (Resistance 3 on macOS showed no need; re-check on the Windows/Linux hosts):

- 0001: (`PS3_LOG_FIRST` saturating log counters are in the toolkit since v0.17.0, #399); savedata root under `PS3_HDD0_ROOT` and `PS3_SAVEDATA_NODATA_OK`
  (upstream's `./gamedata/dev_hdd0/...` default is the same place); `PS3_DEV_FLASH` root list; offline NetStartDialog
  answer (cellNetCtl).
- 0002: SPU LS call hooks and `SPU_GETLOG`; WAIT_SIGNAL queue-ready wake, taskset generations and shared lock helpers
  (spu_workload.c).
- 0003: cellAudio diagnostics (`AUDIO_PEAK`, `AUDIO_WAV`), one pacing clock, ideal-block pacing on Windows (WASAPI).
- 0004: SDL gamepad on its own thread (hot-plug after start).
- 0005: display-list copy at the put store (`GCM_SNAPSHOT_CALLS`), GPU-kept NV3089 resolves, whole-surface readback
  cache, vertex-program analysis cache (performance; not measurable in Resistance 3's first level).
- 0007: FXAA/anisotropy of the live-draw path (the toolkit has `PS3_AA`, `PS3_ANISO`), present dump.
- 0010: optional LGPL-free ATRAC build, MinGW GCC fixes in ppu_loader.cpp.
- 0012: a higher-priority new thread runs until it blocks (SC2HD's audio mixer race; Resistance 3's audio queue key was
  never zero in any run).
