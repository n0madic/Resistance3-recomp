# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests of the launcher: python3 tests/test_launcher.py"""
from __future__ import annotations

import dataclasses
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launcher"))

from r3_launcher import cli, devhost, game, lift, module, profiles, runner, toolchain  # noqa: E402


def make_sfo(**fields: str) -> bytes:
    """A PARAM.SFO with string entries (format 0x204)."""
    keys, data, entries = b"", b"", []
    for name, value in sorted(fields.items()):
        raw = value.encode() + b"\0"
        entries.append((len(keys), len(raw), len(data)))
        keys += name.encode() + b"\0"
        data += raw
    key_off = 20 + 16 * len(entries)
    data_off = key_off + len(keys)
    out = b"\0PSF" + struct.pack("<IIII", 0x101, key_off, data_off, len(entries))
    for k, length, d in entries:
        out += struct.pack("<HHIII", k, 0x204, length, length, d)
    return out + keys + data


def make_disc(root: Path, title: str = "BCES01118", version: str = "01.00", folder: str = "PS3_GAME") -> Path:
    ps3 = root / folder
    (ps3 / "USRDIR" / "PACKED").mkdir(parents=True)
    (ps3 / "PARAM.SFO").write_bytes(make_sfo(TITLE_ID=title, APP_VER=version))
    (ps3 / "USRDIR" / "EBOOT.BIN").write_bytes(b"SCE\0 not really")
    (ps3 / "USRDIR" / "PACKED" / "A.PSARC").write_bytes(b"data")
    (ps3 / "ICON0.PNG").write_bytes(b"png")
    return ps3


class TmpTest(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.tmp = Path(self._td.name).resolve()
        self.addCleanup(self._td.cleanup)


class ProfilesTest(unittest.TestCase):
    def test_known_version(self):
        p = profiles.find_profile("BCES01118", "01.00")
        self.assertIsNotNone(p)
        self.assertEqual(p.work_name, "BCES01118-01.00")
        self.assertEqual(p.code_end, 0xC9DA84)
        self.assertEqual(p.wws_job_extra_entries, {"8f987d5458b2": (0x5318,)})

    def test_unlock_fps_patches(self):
        p = profiles.find_profile("BCES01118", "01.00")
        self.assertEqual({(x.addr, x.orig, x.new) for x in p.insn_patches},
                         {(0x00736C60, 0xEC2107F2, 0xFC200818), (0x00736CF0, 0xEC2107F2, 0xFC200818),
                          (0x007EEAC8, 0xC03F0004, 0xFC210828)})
        self.assertEqual({x.guard for x in p.insn_patches}, {"r3_unlock_fps"})   # host/r3_fps.cpp defines it
        self.assertEqual(p.insn_patches[0].to_json()["addr"], "0x00736c60")

    def test_unknown_version(self):
        self.assertIsNone(profiles.find_profile("BCES01118", "01.01"))
        self.assertIsNone(profiles.find_profile("BLUS00000", "01.00"))
        self.assertIn("BCES01118 01.00", profiles.supported())


class GameFolderTest(TmpTest):
    def test_sfo_roundtrip(self):
        path = self.tmp / "PARAM.SFO"
        path.write_bytes(make_sfo(TITLE_ID="BCES01118", APP_VER="01.00", TITLE="Resistance 3"))
        info = game.read_sfo(path)
        self.assertEqual((info["TITLE_ID"], info["APP_VER"], info["TITLE"]), ("BCES01118", "01.00", "Resistance 3"))

    def test_not_an_sfo(self):
        path = self.tmp / "x"
        path.write_bytes(b"nope" * 10)
        with self.assertRaises(game.GameError):
            game.read_sfo(path)

    def test_folder_and_its_parent(self):
        ps3 = make_disc(self.tmp)
        for given in (ps3, self.tmp):
            g = game.open_game_folder(given)
            self.assertEqual((g.root, g.title_id, g.version), (ps3.resolve(), "BCES01118", "01.00"))
            self.assertEqual(g.eboot_bin, ps3.resolve() / "USRDIR" / "EBOOT.BIN")

    def test_not_a_disc_folder(self):
        (self.tmp / "empty").mkdir()
        for bad in (self.tmp / "missing", self.tmp / "empty"):
            with self.assertRaises(game.GameError):
                game.open_game_folder(bad)

    def test_names_as_the_dump_has_them(self):
        ps3 = make_disc(self.tmp, folder="ps3_game")
        (ps3 / "PARAM.SFO").rename(ps3 / "param.sfo")
        (ps3 / "USRDIR").rename(ps3 / "usrdir")
        (ps3 / "usrdir" / "EBOOT.BIN").rename(ps3 / "usrdir" / "eboot.bin")
        g = game.open_game_folder(self.tmp)
        self.assertEqual(g.eboot_bin.read_bytes(), b"SCE\0 not really")   # the real entries, not PARAM.SFO / USRDIR/EBOOT.BIN
        self.assertEqual(game.read_sfo(g.param_sfo)["TITLE_ID"], "BCES01118")

    def test_no_eboot(self):
        ps3 = make_disc(self.tmp)
        (ps3 / "USRDIR" / "EBOOT.BIN").unlink()
        with self.assertRaisesRegex(game.GameError, "EBOOT.BIN"):
            game.open_game_folder(ps3)

    def test_remember_and_recall(self):
        home = game.Home(self.tmp / "home")
        self.assertIsNone(game.recall(home))
        g = game.open_game_folder(make_disc(self.tmp))
        game.remember(home, g)
        self.assertEqual(game.recall(home), g.root)

    def test_home_layout_matches_the_checkout(self):
        home = game.Home(Path("/h"))
        self.assertEqual(home.hdd0, Path("/h/gamedata/dev_hdd0"))
        self.assertEqual(home.msl_cache, Path("/h/cache/msl"))
        self.assertEqual(home.controls, Path("/h/controls.txt"))
        self.assertEqual(home.work(profiles.find_profile("BCES01118", "01.00")), Path("/h/games/BCES01118-01.00"))

    def test_default_home(self):
        with mock.patch.dict(os.environ, {"R3_HOME": str(self.tmp / "x")}):
            self.assertEqual(game.default_home(Path("/rel")), self.tmp / "x")
        with mock.patch.dict(os.environ, clear=False):
            os.environ.pop("R3_HOME", None)
            self.assertEqual(game.default_home(self.tmp), self.tmp / "data")


class DiscTest(TmpTest):
    def test_layout(self):
        g = game.open_game_folder(make_disc(self.tmp / "dump"))
        (g.root / "USRDIR" / "EBOOT.elf").write_bytes(b"old decrypted copy")   # from the earlier tool: not followed
        home = game.Home(self.tmp / "home")
        elf = home.root / "games" / "x" / "EBOOT.elf"
        elf.parent.mkdir(parents=True)
        elf.write_bytes(b"\x7fELF")
        root = game.build_disc(home, g, elf)
        self.assertEqual(root, home.disc)
        ps3 = home.disc / "PS3_GAME"
        self.assertTrue((ps3 / "USRDIR").is_dir() and not (ps3 / "USRDIR").is_symlink())
        self.assertEqual((ps3 / "PARAM.SFO").read_bytes(), g.param_sfo.read_bytes())
        self.assertEqual((ps3 / "USRDIR" / "PACKED" / "A.PSARC").read_bytes(), b"data")
        self.assertEqual((ps3 / "USRDIR" / "EBOOT.BIN").read_bytes(), g.eboot_bin.read_bytes())
        self.assertEqual((ps3 / "USRDIR" / "EBOOT.elf").resolve(), elf.resolve())
        self.assertEqual((g.root / "USRDIR" / "EBOOT.elf").read_bytes(), b"old decrypted copy")   # the user's file as it was
        game.build_disc(home, g, elf)   # again: the same
        self.assertEqual((ps3 / "USRDIR" / "EBOOT.elf").resolve(), elf.resolve())


class DecryptTest(TmpTest):
    def fake_host(self, body: str) -> Path:
        host = self.tmp / "fake_host"
        host.write_text("#!/bin/sh\n" + body)
        host.chmod(0o755)
        return host

    def test_runs_once_per_eboot(self):
        g = game.open_game_folder(make_disc(self.tmp / "dump"))
        host = self.fake_host('[ "$1" = unself ] || exit 9\nprintf ELF > "$4"\ncount="${0}.count"\necho x >> "$count"\n')
        elf = self.tmp / "out" / "EBOOT.elf"
        self.assertTrue(game.decrypt_eboot(host, g, elf))
        self.assertEqual(elf.read_bytes(), b"ELF")
        self.assertFalse(game.decrypt_eboot(host, g, elf))
        self.assertEqual(len((self.tmp / "fake_host.count").read_text().split()), 1)
        g.eboot_bin.write_bytes(b"SCE\0 another dump")
        self.assertTrue(game.decrypt_eboot(host, g, elf))

    def test_failure_names_the_reason(self):
        g = game.open_game_folder(make_disc(self.tmp / "dump"))
        host = self.fake_host('echo "r3_host unself: error: not a SELF" >&2\nexit 1\n')
        elf = self.tmp / "EBOOT.elf"
        with self.assertRaisesRegex(game.GameError, "not a SELF"):
            game.decrypt_eboot(host, g, elf)
        self.assertFalse(elf.with_name("EBOOT.elf.stamp").exists())


class RunnerTest(TmpTest):
    def setUp(self):
        super().setUp()
        self.home = game.Home(self.tmp / "home")
        self.game = game.open_game_folder(make_disc(self.tmp / "dump"))

    def env(self, **kw):
        return runner.run_environment(self.home, self.game, self.home.disc, runner.RunOptions(**kw))

    def test_paths_are_the_data_directory(self):
        env = self.env()
        self.assertEqual(env["PS3_VFS_ROOT"], str(self.home.disc))
        self.assertEqual(env["PS3_PARAM_SFO"], str(self.game.param_sfo))
        self.assertEqual(env["PS3_HDD0_ROOT"], str(self.home.hdd0))
        self.assertEqual(env["PS3_HDD1_ROOT"], str(self.home.root))
        self.assertEqual(env["PS3RECOMP_MSL_CACHE"], str(self.home.msl_cache))
        self.assertEqual(env["R3_CONTROLS"], str(self.home.controls))
        for name in ("R3_FPS", "PS3_LANG", "PS3RECOMP_METAL_WINDOW", "PS3RECOMP_FULLSCREEN", "PAD_MOUSE_SENS",
                     "R3_UNLOCK_FPS"):
            self.assertNotIn(name, env)

    def test_options(self):
        env = self.env(fps=True, lang="ru", window="1600x900", fullscreen=True, mouse_sens=0.3, unlock_fps=True)
        self.assertEqual((env["R3_FPS"], env["PS3_LANG"], env["PS3RECOMP_METAL_WINDOW"], env["PS3RECOMP_FULLSCREEN"],
                          env["PAD_MOUSE_SENS"], env["R3_UNLOCK_FPS"]), ("1", "ru", "1600x900", "1", "0.3", "1"))

    def test_refuses_a_second_host(self):
        with mock.patch.object(runner, "running_pids", return_value=[123]):
            with self.assertRaisesRegex(SystemExit, "already running"):
                runner.start(self.tmp / "host", self.tmp / "m", self.tmp / "e", self.home, {}, runner.RunOptions())

    def test_directories(self):
        runner.prepare_directories(self.home)
        for d in (self.home.hdd0, self.home.cache, self.home.msl_cache):
            self.assertTrue(d.is_dir())


class CliTest(TmpTest):
    def test_flags(self):
        a = cli.parse_args(["--fps", "--log", "--lang", "ru", "--window", "1600x900", "--fullscreen", "--sens", "0.2",
                            "--unlock-fps"])
        self.assertEqual(a.command, "run")
        self.assertEqual(cli.run_options(a), runner.RunOptions(True, True, "ru", "1600x900", True, 0.2, True))
        self.assertEqual(cli.parse_args(["build"]).command, "build")

    def test_bad_flags(self):
        for argv in (["--window", "big"], ["--window", "0x5"], ["--sens", "0"], ["--jobs", "0"], ["nonsense"]):
            with self.subTest(argv=argv), mock.patch("sys.stderr"), self.assertRaises(SystemExit):
                cli.parse_args(argv)

    def test_clean_cache_keeps_the_shaders(self):
        home = game.Home(self.tmp)
        (home.cache / "BCES01118").mkdir(parents=True)
        (home.cache / "BCES01118" / "f").write_text("x")
        home.msl_cache.mkdir()
        (home.msl_cache / "s").write_text("x")
        cli.clean_cache(home, shaders=False)
        self.assertFalse((home.cache / "BCES01118").exists())
        self.assertTrue((home.msl_cache / "s").exists())
        cli.clean_cache(home, shaders=True)
        self.assertFalse(home.msl_cache.exists())

    def test_reset_keeps_the_saves(self):
        home = game.Home(self.tmp)
        for d in (home.games / "w", home.disc / "PS3_GAME", home.hdd0):
            d.mkdir(parents=True)
        (home.hdd0 / "save").write_text("mine")
        home.game_json.write_text("{}")
        cli.reset(home, everything=False, yes=False)
        self.assertFalse(home.games.exists() or home.disc.exists() or home.game_json.exists())
        self.assertEqual((home.hdd0 / "save").read_text(), "mine")
        with self.assertRaises(SystemExit):   # not a terminal: needs --yes
            with mock.patch("sys.stdin.isatty", return_value=False):
                cli.reset(home, everything=True, yes=False)
        self.assertTrue((home.hdd0 / "save").exists())
        cli.reset(home, everything=True, yes=True)
        self.assertFalse((self.tmp / "gamedata").exists())

    def test_unsupported_version_is_named(self):
        home = game.Home(self.tmp / "home")
        host_dir = self.tmp / "host"
        (host_dir / "sdk").mkdir(parents=True)
        (host_dir / "sdk" / "ABI").write_text("abi")
        (host_dir / "r3_host").write_text("")
        ps3 = make_disc(self.tmp / "dump", version="01.01")
        args = cli.parse_args(["--host-dir", str(host_dir), "--game", str(ps3)])
        with self.assertRaisesRegex(SystemExit, "not a supported game version.*BCES01118 01.00"):
            cli.make_context(args, home)

    def test_no_game_yet(self):
        home = game.Home(self.tmp / "home")
        with self.assertRaisesRegex(SystemExit, "r3.py --game"):
            cli.resolve_game(home, None)

    def test_relative_host_dir_survives_the_chdir(self):
        (self.tmp / "h" / "sdk").mkdir(parents=True)
        (self.tmp / "h" / "sdk" / "ABI").write_text("abi")
        (self.tmp / "h" / "r3_host").write_text("")
        before = Path.cwd()
        self.addCleanup(os.chdir, before)
        os.chdir(self.tmp)
        exe, sdk = cli.host_paths(Path("h"))
        os.chdir(self.tmp / "h" / "sdk")   # what runner.start does before it starts the host: the paths must still be valid
        self.assertTrue(exe.is_absolute() and exe.exists() and sdk.is_absolute() and (sdk / "ABI").exists())

    def test_nothing_is_built_under_a_running_host(self):
        for command in ("build", "run"):
            with self.subTest(command=command), mock.patch.object(runner, "running_pids", return_value=[77]), \
                    mock.patch.object(cli, "make_context") as ctx, mock.patch.object(cli, "build") as build:
                with self.assertRaisesRegex(SystemExit, "already running"):
                    cli.main(["--data", str(self.tmp), command])
                ctx.assert_not_called()
                build.assert_not_called()


class ToolchainTest(TmpTest):
    def test_apple_clang_is_found(self):
        with mock.patch.object(toolchain, "command_line_tools_present", return_value=True), \
                mock.patch.object(toolchain, "_system_clang_ok", return_value=True):
            c = toolchain.find_compiler()
        self.assertTrue(c.cxx[0].endswith("clang++") or c.cxx[0].endswith("c++"))
        self.assertEqual(toolchain.describe(c), c.cxx[0])

    def test_no_compiler_names_the_fix(self):
        with mock.patch.object(toolchain, "command_line_tools_present", return_value=False):
            with self.assertRaisesRegex(toolchain.ToolchainError, "xcode-select --install"):
                toolchain.find_compiler()
        with mock.patch.object(toolchain, "command_line_tools_present", return_value=True), \
                mock.patch.object(toolchain, "_system_clang_ok", return_value=False):   # a clang too old for the lift
            with self.assertRaises(toolchain.ToolchainError):
                toolchain.find_compiler()


class DevHostTest(TmpTest):
    def make_checkout(self):
        for f in ("scripts/build_host.sh", "scripts/lib_patches.sh", "scripts/make_host_header.py", "host/CMakeLists.txt",
                  "host/main.cpp", "patches/domains.txt"):
            (self.tmp / f).parent.mkdir(parents=True, exist_ok=True)
            (self.tmp / f).write_text("x")
        host_dir = devhost.dist_host_dir(self.tmp)
        (host_dir / "sdk").mkdir(parents=True)
        (host_dir / "sdk" / "ABI").write_text("abi")
        (host_dir / "r3_host").write_text("x")
        return host_dir

    def test_staleness(self):
        self.assertTrue(devhost.is_checkout(self.tmp) is False)
        host_dir = self.make_checkout()
        self.assertTrue(devhost.is_checkout(self.tmp))
        now = (host_dir / "r3_host").stat().st_mtime
        os.utime(host_dir / "r3_host", (now + 100, now + 100))
        self.assertFalse(devhost.is_stale(self.tmp, host_dir))
        os.utime(self.tmp / "host" / "main.cpp", (now + 200, now + 200))
        self.assertTrue(devhost.is_stale(self.tmp, host_dir))
        (host_dir / "r3_host").unlink()
        self.assertTrue(devhost.is_stale(self.tmp, host_dir))


class LiftTest(TmpTest):
    def sdk(self) -> Path:
        sdk = self.tmp / "sdk"
        sdk.mkdir()
        (sdk / "ABI").write_text("abi1\n")
        return sdk

    def test_stamp_skips_and_follows_inputs(self):
        sdk, elf, work = self.sdk(), self.tmp / "EBOOT.elf", self.tmp / "work"
        elf.write_bytes(b"\x7fELF one")
        profile = profiles.find_profile("BCES01118", "01.00")

        def fake_lift(sdk_, elf_, work_, profile_):
            (work_ / "recompiled").mkdir(parents=True, exist_ok=True)
            (work_ / "recompiled" / "ppu_recomp.h").write_text("")
            return lift.LiftResult(work_ / "recompiled", work_ / "spu")

        with mock.patch.object(lift, "lift_game", side_effect=fake_lift) as m:
            _, ran = lift.lift_stage(sdk, elf, work, profile)
            self.assertTrue(ran)
            _, ran = lift.lift_stage(sdk, elf, work, profile)
            self.assertFalse(ran)
            self.assertEqual(m.call_count, 1)
            (sdk / "ABI").write_text("abi2\n")   # a new host: the lift is redone
            self.assertTrue(lift.lift_stage(sdk, elf, work, profile)[1])
            elf.write_bytes(b"\x7fELF two")
            self.assertTrue(lift.lift_stage(sdk, elf, work, profile)[1])
            self.assertEqual(m.call_count, 3)
            other = dataclasses.replace(profile, insn_patches=profile.insn_patches[:1])   # another patch set
            self.assertTrue(lift.lift_stage(sdk, elf, work, other)[1])

    def test_insn_patches_reach_the_lifter(self):
        sdk, elf, work = self.sdk(), self.tmp / "EBOOT.elf", self.tmp / "work"
        elf.write_bytes(b"\x7fELF")
        profile = profiles.find_profile("BCES01118", "01.00")
        calls = []

        def fake_tool(script, *args, log_file, env=None):
            calls.append((script.name, [str(a) for a in args]))
            if script.name == "ppu_lifter.py":
                raise lift.LiftError("stop after the PPU lift")

        with mock.patch.object(lift, "_tool", side_effect=fake_tool), self.assertRaises(lift.LiftError):
            lift.lift_game(sdk, elf, work, profile)
        args = dict(calls)["ppu_lifter.py"]
        spec = Path(args[args.index("--insn-patches") + 1])
        self.assertEqual(json.loads(spec.read_text()),
                         {"patches": [p.to_json() for p in profile.insn_patches]})

    def test_ig_regions(self):
        spu = self.tmp / "spu"
        spu.mkdir()
        (spu / "mod.bin").write_bytes(bytes(range(8)))
        manifest = spu / "m.json"
        manifest.write_text(json.dumps([
            {"name": "a", "ea": 0x100000, "span": 0x200},
            {"name": "b", "ea": 0x200000, "span": 0x100, "load_lsa": 0x6600, "bytes": "mod.bin", "code_offset": 4},
            {"name": "c", "ea": 0x300000, "span": 0x100, "load_lsa": 0x100, "bytes": "mod.bin", "pic": True},
        ]))
        workloads = spu / "w.c"
        workloads.write_text("spu_begin_image(1); a_spu_recomp_register();\nspu_begin_image(2); b_spu_recomp_register();\n"
                             "spu_begin_image(3); c_spu_recomp_register();\n")
        out = spu / "igjob_regions.c"
        lift.write_ig_regions(manifest, workloads, out)
        text = out.read_text()
        self.assertIn("spu_overlay_register_region(0x00100000u, 0x200u, 1);", text)
        self.assertIn("b_code[4] = {0x04,0x05,0x06,0x07}", text)
        self.assertIn("spu_overlay_register_verified_region(0x00200000u, 0x6600u, 0x100u, 2", text)
        self.assertIn("spu_overlay_register_pic_region(0x00300000u, 0x100u, 0x100u, 3", text)
        workloads.write_text("spu_begin_image(1); a_spu_recomp_register();\n")
        with self.assertRaises(lift.LiftError):
            lift.write_ig_regions(manifest, workloads, out)


class ModuleTest(TmpTest):
    def test_units(self):
        sdk, ppu, spu = self.tmp / "sdk", self.tmp / "ppu", self.tmp / "spu"
        (sdk / "runtime" / "spu").mkdir(parents=True)
        (sdk / "runtime" / "spu" / "spu_dispatch_mt.c").write_text("")
        ppu.mkdir()
        (spu / "img1").mkdir(parents=True)
        (spu / "img1" / "spu_recomp.c").write_text("")
        for n in ("spu_workloads.c", "jobmods_register.c", "igjob_regions.c"):
            (spu / n).write_text("")
        for n in ("ppu_recomp_001.cpp", "ppu_recomp_000.cpp", "other.cpp"):
            (ppu / n).write_text("")
        units = module._units(module.ModuleInputs(sdk, ppu, spu, self.tmp / "out"))
        self.assertEqual([u[1].name for u in units if u[0] == "c++"], ["ppu_recomp_000.cpp", "ppu_recomp_001.cpp"])
        self.assertEqual([u[1].name for u in units if u[0] == "c"],
                         ["spu_recomp.c", "spu_workloads.c", "jobmods_register.c", "igjob_regions.c", "spu_dispatch_mt.c"])
        with self.assertRaises(module.ModuleBuildError):
            module._units(module.ModuleInputs(sdk, self.tmp / "none", spu, self.tmp / "out"))

    def fake_build(self):
        """Inputs for build_module and a compiler that only records its calls (the log) and writes its -o file. With
        FAKE_CC_FAIL=<text> in the environment, a command containing the text fails and every other one takes 0.3 s."""
        sdk, ppu, spu = self.tmp / "sdk", self.tmp / "ppu", self.tmp / "spu"
        (sdk / "runtime" / "spu").mkdir(parents=True)
        (sdk / "runtime" / "spu" / "spu_dispatch_mt.c").write_text("dispatch")
        (sdk / "ABI").write_text("abi1\n")
        ppu.mkdir()
        (spu / "img1").mkdir(parents=True)
        (spu / "img1" / "spu_recomp.c").write_text("spu")
        for n in ("ppu_recomp_000.cpp", "ppu_recomp_001.cpp", "ppu_recomp_002.cpp"):
            (ppu / n).write_text(n)
        for n in ("spu_workloads.c", "jobmods_register.c", "igjob_regions.c"):
            (spu / n).write_text(n)
        script = self.tmp / "fake_cc"
        script.write_text('#!/bin/sh\necho "$*" >> "$0.log"\n'
                          'if [ -n "$FAKE_CC_FAIL" ]; then\n'
                          '  case "$*" in *"$FAKE_CC_FAIL"*) echo "error: boom" >&2; exit 1;; esac\n  sleep 0.3\nfi\n'
                          'out=""\nwhile [ $# -gt 0 ]; do [ "$1" = "-o" ] && out="$2"; shift; done\necho built > "$out"\n')
        script.chmod(0o755)
        cc = toolchain.Compiler((str(script),), (str(script),))
        return module.ModuleInputs(sdk, ppu, spu, self.tmp / "out"), cc, Path(str(script) + ".log")

    def test_objects_and_the_link_are_cached(self):
        inp, cc, log = self.fake_build()
        calls = lambda: len(log.read_text().splitlines())   # noqa: E731
        units = len(module._units(inp)) + 1                  # and the unit holding the ABI string
        out = module.build_module(inp, cc, jobs=3)
        self.assertEqual((out.name, calls()), (module.MODULE_NAME, units + 1))   # every unit, and the link
        module.build_module(inp, cc, jobs=3)
        self.assertEqual(calls(), units + 1)                 # nothing again: no compile and no relink
        (inp.ppu / "ppu_recomp_001.cpp").write_text("another lift")
        module.build_module(inp, cc, jobs=3)
        self.assertEqual(calls(), units + 3)                 # the changed unit, and the link
        out.unlink()
        module.build_module(inp, cc, jobs=3)
        self.assertEqual(calls(), units + 4)                 # only the link
        (inp.sdk / "ABI").write_text("abi2\n")               # a new toolkit: everything
        module.build_module(inp, cc, jobs=3)
        self.assertEqual(calls(), 2 * units + 5)

    def test_a_failed_unit_does_not_compile_the_queue(self):
        inp, cc, log = self.fake_build()
        with mock.patch.dict(os.environ, {"FAKE_CC_FAIL": "ppu_recomp_000.cpp"}):
            with self.assertRaisesRegex(module.ModuleBuildError, "boom"):
                module.build_module(inp, cc, jobs=1)
        self.assertLessEqual(len(log.read_text().splitlines()), 3)   # not the nine units
        self.assertFalse((inp.out / module.MODULE_NAME).exists())


if __name__ == "__main__":
    unittest.main()
