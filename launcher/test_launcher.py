#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for the Hebrew launcher (app.py, deck.sh). Run on Linux or in WSL:

    python3 launcher/test_launcher.py

deck.sh runs against a simulated deck: a settings folder, a fake systemctl on PATH that stages
the stick's azmod.so like the loader and writes the hook table like azmod, and AZH_* paths."""
import http.client
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import app  # noqa: E402

PUB = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIHebrewLauncherKeyxxxxxxxxxxxxxxxxxxxxxxxxx az-hebrew-launcher"
OTHER = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOtherKeyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy someone@pc"


class Keys(unittest.TestCase):
    def test_new_file_gets_our_key(self):
        self.assertEqual(app.merged_authorized_keys(None, "", PUB), PUB + "\n")

    def test_existing_keys_are_kept_and_ours_added_once(self):
        once = app.merged_authorized_keys(OTHER + "\r\n", "", PUB)
        self.assertEqual(once, OTHER + "\n" + PUB + "\n")
        self.assertEqual(app.merged_authorized_keys(once, "", PUB), once)

    def test_same_key_with_another_comment_is_not_added_again(self):
        text = PUB.rsplit(" ", 1)[0] + " renamed\n"
        self.assertEqual(app.merged_authorized_keys(text, "", PUB), text)

    def test_inline_keys_in_ssh_enable_move_into_the_file(self):
        out = app.merged_authorized_keys(None, "# on\n" + OTHER + "\n", PUB)
        self.assertEqual(out, OTHER + "\n" + PUB + "\n")

    def test_stick_keys_prefers_authorized_keys(self):
        with tempfile.TemporaryDirectory() as d:
            mods = Path(d)
            (mods / "ssh.enable").write_text(OTHER + "\n")
            self.assertEqual(app.stick_keys(mods), {app.pub_key_body(OTHER)})
            (mods / "authorized_keys").write_text(PUB + "\n")
            self.assertEqual(app.stick_keys(mods), {app.pub_key_body(PUB)})


class Ssh(unittest.TestCase):
    def test_error_kinds(self):
        self.assertEqual(app.ssh_error_kind("root@10.0.0.2: Permission denied (publickey)."), "denied")
        self.assertEqual(app.ssh_error_kind("ssh: connect to host 10.0.0.2 port 22: Connection timed out"), "unreachable")
        self.assertEqual(app.ssh_error_kind("@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @"), "hostkey")
        self.assertEqual(app.ssh_error_kind("kex_exchange_identification: read: Connection reset"), "ssh")

    def test_ssh_command_pins_key_and_known_hosts(self):
        cmd = app.ssh_cmd("10.0.0.2", "true")
        self.assertIn("BatchMode=yes", cmd)
        self.assertIn("UserKnownHostsFile=" + str(app.KNOWN), cmd)
        self.assertEqual(cmd[-2:], ["root@10.0.0.2", "true"])


class Source(unittest.TestCase):
    def test_version_and_glyphs(self):
        self.assertRegex(app.source_version(), r"^\d+\.\d+\.\d+$")
        g = app.glyph_paths()
        self.assertEqual(g["upm"], 2048)
        self.assertTrue(g["glyphs"][0x05E9]["d"].startswith("M"))  # shin
        self.assertEqual(g["glyphs"][0x05B8]["d"], "")  # qamats: a mark, no outline
        for cp, v in g["glyphs"].items():
            self.assertEqual(v["d"].count("M"), v["d"].count("Z"), hex(cp))

    def test_fingerprint_follows_the_source(self):
        a = app.source_fingerprint()
        self.assertEqual(a, app.source_fingerprint())


class Stick(unittest.TestCase):
    """do_install / do_restore_stick on a folder standing in for the stick."""

    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.mods = self.t / "MODS"
        (self.mods / "deck").mkdir(parents=True)
        (self.mods / "deck" / "version").write_text(app.source_version() + "\n")
        (self.mods / "azmod.so").write_bytes(b"official")
        (self.mods / "ssh.enable").write_text(OTHER + "\n")
        out = self.t / "build" / "out"
        out.mkdir(parents=True)
        (out / "azmod.so").write_bytes(b"ours")
        (self.t / "official.so").write_bytes(b"official")
        self.saved = {k: getattr(app, k) for k in ("stick_mods", "BUILD_DIR", "build_state", "ensure_key",
                                                    "set_setting", "official_set", "public_key")}
        app.stick_mods = lambda letter: self.mods
        app.BUILD_DIR = self.t / "build"
        app.build_state = lambda v: {"sha": app.sha256(out / "azmod.so"), "current": True}
        app.ensure_key = lambda: PUB
        app.public_key = lambda: PUB
        app.set_setting = lambda **kw: None
        app.official_set = lambda v: {"azmod": self.t / "official.so", "sha": app.sha256(self.t / "official.so")}

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(app, k, v)
        shutil.rmtree(self.t)

    def test_install_puts_the_build_and_the_key_on_the_stick(self):
        job = app.Job("install")
        app.App().do_install(job, "X")
        self.assertIsNone(job.error)
        self.assertEqual((self.mods / "azmod.so").read_bytes(), b"ours")
        self.assertEqual((self.mods / "authorized_keys").read_text(), OTHER + "\n" + PUB + "\n")
        self.assertEqual((self.mods / "ssh.enable").read_text(), OTHER + "\n")
        self.assertFalse(list(self.mods.glob("*.az-hebrew")))
        st = app.stick_state("X", app.sha256(self.mods / "azmod.so"), None, PUB)
        self.assertEqual((st["has"], st["ssh"]), ("ours", True))

    def test_a_stick_of_another_release_is_left_alone(self):
        (self.mods / "deck" / "version").write_text("1.0.9\n")
        job = app.Job("install")
        app.App().do_install(job, "X")
        self.assertEqual(job.error["code"], "stick-version")
        self.assertEqual((self.mods / "azmod.so").read_bytes(), b"official")

    def test_restore_puts_the_official_file_back(self):
        app.App().do_install(app.Job("install"), "X")
        job = app.Job("restore-stick")
        app.App().do_restore_stick(job, "X")
        self.assertIsNone(job.error)
        self.assertEqual((self.mods / "azmod.so").read_bytes(), b"official")


class Server(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = app.ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        cls.srv.app = app.App()
        cls.srv.glyphs = {"upm": 2048, "glyphs": {}}
        cls.port = cls.srv.server_address[1]
        cls.srv.authority = "127.0.0.1:%d" % cls.port
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def get(self, path, token=None, host=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        h = {"Host": host or self.srv.authority}
        if token:
            h["Authorization"] = "Bearer " + token
        c.request("GET", path, headers=h)
        r = c.getresponse()
        return r.status, r.read()

    def test_page_without_token_api_with_token_only(self):
        self.assertEqual(self.get("/")[0], 200)
        self.assertEqual(self.get("/api/glyphs")[0], 403)
        self.assertEqual(self.get("/api/glyphs", token="wrong")[0], 403)
        self.assertEqual(self.get("/api/glyphs", token=self.srv.app.token)[0], 200)

    def test_another_host_name_is_refused(self):
        self.assertEqual(self.get("/", host="evil.example:%d" % self.port)[0], 403)
        self.assertEqual(self.get("/api/ping", token=self.srv.app.token, host="localhost:%d" % self.port)[0], 403)

    def test_unknown_post_is_refused(self):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request("POST", "/api/nope", body=b"{}", headers={"Host": self.srv.authority,
                  "Authorization": "Bearer " + self.srv.app.token, "Content-Type": "application/json"})
        self.assertEqual(c.getresponse().status, 400)


@unittest.skipUnless(shutil.which("sh") and os.name != "nt", "needs a POSIX sh (Linux or WSL)")
class DeckScript(unittest.TestCase):
    SHA = "ab" * 32

    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.settings = self.t / "settings"
        (self.settings / "az-usbload").mkdir(parents=True)
        (self.t / "release").write_text("2.01\n")
        self.stick = self.t / "stick-azmod.so"
        self.stick.write_bytes(b"ours")
        bin_ = self.t / "bin"
        bin_.mkdir()
        # the loader stages the stick's file; azmod writes its hook table when EP147 starts
        (bin_ / "systemctl").write_text(
            "#!/bin/sh\necho \"$@\" >> \"$T/calls\"\n"
            "case \"$1 $2\" in\n"
            "  'restart az-usbload.service') [ -f \"$T/nostage\" ] || cp \"$T/stick-azmod.so\" \"$T/settings/az-usbload/azmod.so\" ;;\n"
            "  'start EP147.service') [ -f \"$T/nohooks\" ] || printf 'hooks 3\\n0x2554ee0 island       ctor  AZ-MODS    heb_line_hook\\n"
            "0x1dc17d8 island       ctor  AZ-MODS    hk_ctor_hook\\n0x1dc6428 island       ctor  AZ-MODS    hk_dtor_hook\\n' > \"$T/hooktable\" ;;\n"
            "esac\n")
        (bin_ / "sha256sum").write_text(  # the stick's bytes stand for the build the launcher made
            "#!/bin/sh\n[ -f \"$1\" ] || exit 1\nif [ \"$(cat \"$1\")\" = ours ]; then echo \"" + "ab" * 32 + "  $1\"; "
            "else echo \"" + "cd" * 32 + "  $1\"; fi\n")
        for f in bin_.iterdir():
            f.chmod(f.stat().st_mode | stat.S_IEXEC)
        self.env = dict(os.environ, T=str(self.t), PATH=str(bin_) + ":" + os.environ["PATH"],
                        AZH_SETTINGS=str(self.settings), AZH_HOOKTABLE=str(self.t / "hooktable"),
                        AZH_RELEASE=str(self.t / "release"), AZH_LOG=str(self.t / "log"), AZH_WAIT="0")

    def tearDown(self):
        shutil.rmtree(self.t)

    def deck(self, mode="apply", sha=None):
        subprocess.run(["sh", str(HERE / "deck.sh"), mode, sha or self.SHA], env=self.env, timeout=60)
        return [l for l in (self.t / "log").read_text().splitlines() if l.startswith("@@ ")]

    def conf(self, text):
        (self.settings / "azmod.conf").write_text(text)

    def test_apply_turns_hebrew_on_and_waits_for_the_hooks(self):
        self.conf("# AZ-MODS settings\nversion = 1\nstems = on\n")
        out = self.deck()
        self.assertEqual(out[-1], "@@ done ok on 2")
        self.assertTrue((self.settings / "azmods-unsigned.on").exists())
        self.assertEqual((self.settings / "azmod.conf").read_text(),
                         "# AZ-MODS settings\nversion = 1\nstems = on\nhebrew = on\n")
        calls = (self.t / "calls").read_text().split("\n")
        self.assertLess(calls.index("stop EP147.service"), calls.index("start EP147.service"))

    def test_an_existing_hebrew_line_is_replaced_and_its_children_kept(self):
        self.conf("version = 1\nhebrew = off\nhebrew-rtl = off\n")
        self.deck()
        self.assertEqual((self.settings / "azmod.conf").read_text(), "version = 1\nhebrew = on\nhebrew-rtl = off\n")

    def test_a_last_line_without_newline_is_not_joined(self):
        self.conf("version = 1\nstems = on")
        self.deck()
        self.assertEqual((self.settings / "azmod.conf").read_text(), "version = 1\nstems = on\nhebrew = on\n")

    def test_no_conf_or_a_newer_one_is_left_alone(self):
        out = self.deck()
        self.assertEqual(out[-1], "@@ done ok skip 2")
        self.assertFalse((self.settings / "azmod.conf").exists())
        self.conf("version = 2\n")
        self.deck()
        self.assertEqual((self.settings / "azmod.conf").read_text(), "version = 2\n")

    def test_no_hooks_is_reported_not_hidden(self):
        (self.t / "nohooks").write_text("")
        self.conf("version = 1\n")
        self.assertEqual(self.deck()[-1], "@@ done nohooks on")

    def test_another_stick_in_the_deck_times_out(self):
        (self.t / "nostage").write_text("")
        self.assertEqual(self.deck()[-1], "@@ error stage-timeout")
        self.assertNotIn("stop EP147.service", (self.t / "calls").read_text())

    def test_restore_removes_the_override(self):
        (self.settings / "azmods-unsigned.on").write_text("")
        self.stick.write_bytes(b"official")
        out = self.deck("restore", "cd" * 32)
        self.assertEqual(out[-1], "@@ done restored")
        self.assertFalse((self.settings / "azmods-unsigned.on").exists())

    def test_bad_arguments_change_nothing(self):
        self.assertEqual(self.deck("apply", "nothex")[-1], "@@ error usage")
        self.assertFalse((self.t / "calls").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
