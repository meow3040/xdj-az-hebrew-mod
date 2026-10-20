#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""AZ Hebrew launcher: build AZ-MODS with the Hebrew mod, put it on the USB stick, turn it on
at the deck. Double-click AZ-Hebrew-Launcher.cmd (this repository's root) on Windows.

The window is a local page (127.0.0.1, a fresh token per start) shown in Edge or Chrome's app
mode. Three steps, each a job of this process:

  build    launcher/build-azmod.sh in WSL: the AZ-MODS source azmods/BASE names, with the
           mod's files and patch (azmods/), and the official AZ-MODS set of the same version,
           which the AZ-MODS app keeps in %APPDATA%\\AZ-MODS\\mods\\<version>. The result
           goes to the launcher's own folder.
  install  copies that azmod.so over MODS\\azmod.so on a stick the AZ-MODS app set up, and
           lets the launcher's SSH key in (MODS\\authorized_keys and MODS\\ssh.enable).
  deck     over SSH (the deck asks once to allow the key): deck.sh apply, which allows unsigned
           builds on this deck, stages the stick's set, turns Hebrew on and starts the deck
           software again. restore puts the official azmod.so back and removes the permission.

Everything the launcher keeps is in %LOCALAPPDATA%\\AZ-Hebrew-Launcher. Standard library only.
"""
import base64
import concurrent.futures
import ctypes
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = Path(__file__).resolve().parent
MOD = HERE.parent  # this repository: azmods/ (the mod) and launcher/
WEB = HERE / "web"
DATA = Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local") / "AZ-Hebrew-Launcher"
APPDATA = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
NOWIN = 0x08000000 if os.name == "nt" else 0  # CREATE_NO_WINDOW
IDLE_EXIT = 600  # seconds without a request (window closed) and no job running

# ---------------------------------------------------------------- small helpers
def log(msg):
    DATA.mkdir(parents=True, exist_ok=True)
    p = DATA / "launcher.log"
    try:
        if p.exists() and p.stat().st_size > 512 * 1024:
            p.replace(p.with_suffix(".log.1"))
        with p.open("a", encoding="utf-8") as f:
            f.write(time.strftime("[%H:%M:%S] ") + msg.rstrip() + "\n")
    except OSError:
        pass


_sha_cache = {}


def sha256(path):
    """SHA-256 of a file, remembered by size and modification time (the page asks every second)."""
    st = os.stat(path)
    key = (str(path), st.st_size, st.st_mtime_ns)
    if key not in _sha_cache:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for b in iter(lambda: f.read(1 << 20), b""):
                h.update(b)
        if len(_sha_cache) > 64:
            _sha_cache.clear()
        _sha_cache[key] = h.hexdigest()
    return _sha_cache[key]


def run(cmd, timeout=60, data=None):
    return subprocess.run(cmd, input=data, capture_output=True, timeout=timeout, creationflags=NOWIN)


def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def save_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, path)


SETTINGS = DATA / "settings.json"


def settings():
    return load_json(SETTINGS, {})


def set_setting(**kw):
    s = settings()
    s.update(kw)
    save_json(SETTINGS, s)


# ---------------------------------------------------------------- the source and the official set
def base():
    """azmods/BASE: the AZ-MODS repository, commit and version the mod applies to."""
    return read_kv(MOD / "azmods" / "BASE")


def source_version():
    return base().get("version", "?")


def source_fingerprint():
    """What a build depends on: the mod (azmods/) and the build script, by size and time."""
    h = hashlib.sha256()
    for root in (MOD / "azmods", HERE / "build-azmod.sh"):
        files = [root] if root.is_file() else sorted(p for p in root.rglob("*") if p.is_file())
        for p in files:
            st = p.stat()
            h.update(("%s %d %d\n" % (p.relative_to(MOD).as_posix(), st.st_size, st.st_mtime_ns)).encode())
    return h.hexdigest()[:16]


def official_set(version):
    """The AZ-MODS app's verified copy of release `version`: its folder and azmod.so's SHA-256, or None."""
    d = APPDATA / "AZ-MODS" / "mods" / version
    man = load_json(d / "manifest.json", None)
    if not man or man.get("version") != version or not (d / "azmod.so").is_file():
        return None
    want = {f["name"]: f.get("sha256") for e in man.get("entries", []) for f in e.get("files", []) if "dir" not in f}
    got = sha256(d / "azmod.so")
    if want.get("azmod.so") != got:
        return None
    return {"dir": d, "azmod": d / "azmod.so", "sha": got}


def azmods_app():
    for p in (Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "AZ-MODS" / "AZ-MODS.exe",
              Path(os.environ.get("PROGRAMFILES", "")) / "AZ-MODS" / "AZ-MODS.exe"):
        if p.is_file():
            return p
    return None


# ---------------------------------------------------------------- WSL
_wsl_cache = {"t": 0, "v": None}


def wsl_distro():
    """The WSL distribution builds run in (Ubuntu first), or None."""
    if time.time() - _wsl_cache["t"] < 30:
        return _wsl_cache["v"]
    v = None
    exe = shutil.which("wsl.exe") or shutil.which("wsl")
    if exe:
        try:
            out = run([exe, "-l", "-q"], timeout=20).stdout
            names = [n.strip() for n in out.decode("utf-16-le", "ignore").replace("\x00", "").splitlines() if n.strip()]
            names = [n for n in names if not n.lower().startswith("docker-desktop")]
            v = next((n for n in names if n.lower().startswith("ubuntu")), names[0] if names else None)
        except (OSError, subprocess.SubprocessError):
            v = None
    _wsl_cache.update(t=time.time(), v=v)
    return v


def wsl_path(distro, p):
    out = run(["wsl.exe", "-d", distro, "--", "wslpath", "-a", str(p).replace("\\", "/")], timeout=30)
    path = out.stdout.decode("utf-8", "replace").strip()
    if out.returncode or not path.startswith("/"):
        raise RuntimeError("wslpath failed for %s" % p)
    return path


# ---------------------------------------------------------------- the build
BUILD_DIR = DATA / "build"
BUILD_INFO = BUILD_DIR / "build.json"


def build_state(version):
    info = load_json(BUILD_INFO, None)
    so = BUILD_DIR / "out" / "azmod.so"
    if not info or not so.is_file() or info.get("sha") != sha256(so):
        return None
    info["current"] = info.get("fingerprint") == source_fingerprint() and info.get("version") == version
    return info


# ---------------------------------------------------------------- sticks
def drives():
    """Removable drives, and any drive that already holds a MODS folder (never C:)."""
    out = []
    if os.name != "nt":
        return out
    k32 = ctypes.windll.kernel32
    mask = k32.GetLogicalDrives()
    for i in range(26):
        if not mask >> i & 1:
            continue
        letter = chr(65 + i)
        root = letter + ":\\"
        if letter in "AC":
            continue
        kind = k32.GetDriveTypeW(root)
        if kind not in (2, 3):  # removable, fixed (some USB disks report fixed)
            continue
        if kind == 3 and not os.path.isdir(root + "MODS"):
            continue
        label = ctypes.create_unicode_buffer(261)
        if not k32.GetVolumeInformationW(root, label, 261, None, None, None, None, 0):
            continue  # no medium
        try:
            total = shutil.disk_usage(root).total
        except OSError:
            continue
        out.append({"letter": letter, "label": label.value, "gb": round(total / 1e9)})
    return out


def read_kv(path):
    kv = {}
    try:
        for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = v.strip()
    except OSError:
        pass
    return kv


def stick_mods(letter):
    return Path(letter + ":\\") / "MODS"


def stick_state(letter, build_sha, official_sha, pub):
    mods = stick_mods(letter)
    st = {"letter": letter, "azmods": None, "firmware": None, "has": "none", "ssh": False}
    try:
        st["azmods"] = (mods / "deck" / "version").read_text(encoding="utf-8").strip() or None
    except OSError:
        pass
    st["firmware"] = read_kv(mods / ".deck-info").get("firmware")
    so = mods / "azmod.so"
    if so.is_file():
        s = sha256(so)
        st["has"] = "ours" if s == build_sha else "official" if s == official_sha else "other"
    st["ssh"] = bool(pub) and (mods / "ssh.enable").is_file() and pub_key_body(pub) in stick_keys(mods)
    return st


# ---------------------------------------------------------------- SSH
SSH_DIR = DATA / "ssh"
KEY = SSH_DIR / "id_ed25519"
KNOWN = SSH_DIR / "known_hosts"


def openssh(tool):
    sysdir = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "OpenSSH" / (tool + ".exe")
    return str(sysdir) if sysdir.is_file() else (shutil.which(tool) or tool)


def ensure_key():
    """The launcher's own key (made once). Returns the public key line."""
    if not KEY.is_file():
        SSH_DIR.mkdir(parents=True, exist_ok=True)
        r = run([openssh("ssh-keygen"), "-q", "-t", "ed25519", "-N", "", "-C", "az-hebrew-launcher", "-f", str(KEY)])
        if r.returncode or not KEY.is_file():
            raise RuntimeError("ssh-keygen failed: " + r.stderr.decode("utf-8", "replace"))
        if os.name == "nt":  # OpenSSH refuses a private key others can read
            run(["icacls", str(KEY), "/inheritance:r", "/grant:r", "%s:F" % os.environ.get("USERNAME", "")])
    return Path(str(KEY) + ".pub").read_text(encoding="utf-8").strip()


def public_key():
    try:
        return Path(str(KEY) + ".pub").read_text(encoding="utf-8").strip()
    except OSError:
        return None


def pub_key_body(line):
    """`type base64` of a public key line (the comment is not part of the key)."""
    parts = line.split()
    return " ".join(parts[:2]) if len(parts) >= 2 else line.strip()


def stick_keys(mods):
    """The key lines the deck's SSH hook reads from a stick: MODS/authorized_keys, else inline
    in MODS/ssh.enable (deck/stick/deck/boot.d/10-sshgate.sh)."""
    src = mods / "authorized_keys"
    if not src.is_file():
        src = mods / "ssh.enable"
    try:
        text = src.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return set()
    return {pub_key_body(l) for l in text.splitlines() if "AAAA" in l}


def merged_authorized_keys(keys_text, enable_text, pub):
    """MODS/authorized_keys with our key added. When the stick had no authorized_keys
    (keys_text None), the keys written inline in ssh.enable move into it: the hook prefers the
    file, so they must not be left behind."""
    if keys_text is None:
        keys_text = "\n".join(l for l in (enable_text or "").splitlines() if "AAAA" in l)
    lines = [l.rstrip("\r") for l in keys_text.splitlines() if l.strip()]
    if pub_key_body(pub) not in {pub_key_body(l) for l in lines}:
        lines.append(pub)
    return "\n".join(lines) + "\n"


def ssh_cmd(ip, remote):
    return [openssh("ssh"), "-i", str(KEY), "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
            "-o", "StrictHostKeyChecking=accept-new", "-o", "UserKnownHostsFile=" + str(KNOWN),
            "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3",
            "root@" + ip, remote]


def ssh_error_kind(text):
    t = text.lower()
    if "remote host identification has changed" in t or "host key verification failed" in t:
        return "hostkey"
    if "permission denied" in t:
        return "denied"
    if "timed out" in t or "no route" in t or "unreachable" in t or "refused" in t or "could not resolve" in t:
        return "unreachable"
    return "ssh"


def local_subnets():
    """The /24 of the default route's address, plus the computer's other private addresses
    (172.16/12 only on the default route: WSL and Hyper-V put their virtual switches there)."""
    nets = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))  # no packet is sent: this only picks the route
        nets.add(ipaddress.ip_network(s.getsockname()[0] + "/24", strict=False))
        s.close()
    except OSError:
        pass
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    except OSError:
        infos = []
    for a in {i[4][0] for i in infos}:
        ip = ipaddress.ip_address(a)
        if ip.is_private and not ip.is_loopback and not ip.is_link_local and ip not in ipaddress.ip_network("172.16.0.0/12"):
            nets.add(ipaddress.ip_network(a + "/24", strict=False))
    return sorted(nets, key=str)


def dropbear_at(ip):
    try:
        with socket.create_connection((ip, 22), timeout=0.6) as s:
            s.settimeout(1.2)
            return b"dropbear" in s.recv(64).lower()
    except OSError:
        return False


# ---------------------------------------------------------------- jobs
class Job:
    def __init__(self, kind):
        self.kind, self.state, self.step, self.result = kind, "running", "", None
        self.lines, self.error, self.started = [], None, time.time()

    def say(self, line):
        line = line.rstrip()
        if line:
            self.lines.append(line)
            del self.lines[:-400]
            log("%s: %s" % (self.kind, line))

    def view(self):
        return {"kind": self.kind, "state": self.state, "step": self.step, "error": self.error,
                "result": self.result, "lines": self.lines[-120:], "seconds": int(time.time() - self.started)}


class App:
    def __init__(self):
        self.lock = threading.Lock()
        self.job = None
        self.last_seen = time.time()
        self.token = secrets.token_urlsafe(24)

    def start(self, kind, fn, *args):
        with self.lock:
            if self.job and self.job.state == "running":
                return False
            job = self.job = Job(kind)

        def body():
            try:
                fn(job, *args)
                if job.state == "running":
                    job.state = "ok"
            except Exception as e:  # shown to the user, details in the log
                job.state, job.error = "error", job.error or {"code": "internal", "text": str(e)}
                log(traceback.format_exc())
        threading.Thread(target=body, daemon=True).start()
        return True

    # -- state for the page
    def state(self):
        version = source_version()
        off = official_set(version)
        b = build_state(version)
        pub = public_key()
        sticks = []
        for d in drives():
            try:
                st = stick_state(d["letter"], b and b["sha"], off and off["sha"], pub)
            except OSError:
                continue  # pulled out while being read
            st.update(d)
            sticks.append(st)
        s = settings()
        return {"version": version, "official": bool(off), "app": bool(azmods_app()),
                "wsl": wsl_distro(), "build": b, "sticks": sticks, "stick": s.get("stick"),
                "deck_ip": s.get("deck_ip", ""), "deck_done": s.get("deck_done"),
                "job": self.job.view() if self.job else None}

    # -- build
    def do_build(self, job):
        version = source_version()
        off = official_set(version)
        distro = wsl_distro()
        if not distro:
            job.state, job.error = "error", {"code": "no-wsl"}
            return
        if not off:
            job.state, job.error = "error", {"code": "no-official", "version": version}
            return
        out = BUILD_DIR / "out"
        out.mkdir(parents=True, exist_ok=True)
        (DATA / "downloads").mkdir(parents=True, exist_ok=True)
        fp = source_fingerprint()
        args = [wsl_path(distro, p) for p in (HERE / "build-azmod.sh", MOD, off["azmod"], out, DATA / "downloads")]
        p = subprocess.Popen(["wsl.exe", "-d", distro, "--", "bash"] + args, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, creationflags=NOWIN)
        done = None
        for raw in p.stdout:
            line = raw.decode("utf-8", "replace")
            job.say(line)
            m = re.match(r"@@ (\S+) ?(.*)", line.strip())
            if not m:
                continue
            if m.group(1) == "done":
                done = m.group(2).split()
            elif m.group(1) == "error":
                job.error = {"code": "build", "text": m.group(2)}
            else:
                job.step = m.group(1)
        rc = p.wait()
        if rc or not done:
            job.state = "error"
            job.error = job.error or {"code": "build", "text": "exit %d" % rc}
            return
        so = out / "azmod.so"
        if sha256(so) != done[0]:
            job.state, job.error = "error", {"code": "build", "text": "the result changed after the build"}
            return
        info = {"sha": done[0], "size": int(done[1]), "glibc": done[2], "version": version,
                "fingerprint": fp, "time": int(time.time())}
        save_json(BUILD_INFO, info)
        job.result = info

    # -- install to a stick
    def do_install(self, job, letter):
        version = source_version()
        b = build_state(version)
        if not b or not b["current"]:
            job.state, job.error = "error", {"code": "no-build"}
            return
        mods = stick_mods(letter)
        st = stick_state(letter, b["sha"], None, public_key())
        if st["azmods"] != version:
            job.state, job.error = "error", {"code": "stick-version", "have": st["azmods"], "want": version}
            return
        job.step = "copy"
        src, dst = BUILD_DIR / "out" / "azmod.so", mods / "azmod.so"
        tmp = mods / "azmod.so.az-hebrew"
        shutil.copyfile(src, tmp)
        with open(tmp, "rb+") as f:
            os.fsync(f.fileno())
        os.replace(tmp, dst)
        if sha256(dst) != b["sha"]:
            job.state, job.error = "error", {"code": "copy-verify"}
            return
        job.say("azmod.so -> %s (%s)" % (dst, b["sha"][:12]))
        job.step = "ssh"
        pub = ensure_key()
        keys = mods / "authorized_keys"
        enable = mods / "ssh.enable"
        existing = keys.read_text(encoding="utf-8", errors="replace") if keys.is_file() else None
        inline = enable.read_text(encoding="utf-8", errors="replace") if enable.is_file() else ""
        keys.write_text(merged_authorized_keys(existing, inline, pub), encoding="utf-8", newline="\n")
        if not enable.is_file():
            enable.write_text("", encoding="utf-8")
        job.say("SSH key on the stick: %s" % pub_key_body(pub)[:40])
        set_setting(stick=letter, deck_done=None)
        job.result = {"letter": letter}

    def do_restore_stick(self, job, letter):
        version = source_version()
        off = official_set(version)
        mods = stick_mods(letter)
        if not off or not (mods / "deck" / "version").is_file():
            job.state, job.error = "error", {"code": "no-official", "version": version}
            return
        tmp = mods / "azmod.so.az-hebrew"
        shutil.copyfile(off["azmod"], tmp)
        os.replace(tmp, mods / "azmod.so")
        job.say("official azmod.so back on %s:" % letter)
        job.result = {"letter": letter}

    # -- the deck
    def find_deck(self, job):
        job.step = "scan"
        hosts = [str(h) for n in local_subnets() for h in n.hosts()]
        job.say("scanning %d addresses for the deck's SSH" % len(hosts))
        with concurrent.futures.ThreadPoolExecutor(64) as ex:
            found = [h for h, ok in zip(hosts, ex.map(dropbear_at, hosts)) if ok]
        job.result = {"found": found}
        if len(found) == 1:
            set_setting(deck_ip=found[0])

    def deck_run(self, job, ip, mode):
        if not re.fullmatch(r"[0-9]{1,3}(\.[0-9]{1,3}){3}", ip or ""):
            job.state, job.error = "error", {"code": "bad-ip"}
            return
        set_setting(deck_ip=ip)
        version = source_version()
        if mode == "apply":
            b = build_state(version)
            if not b:
                job.state, job.error = "error", {"code": "no-build"}
                return
            want = b["sha"]
        else:
            off = official_set(version)
            if not off:
                job.state, job.error = "error", {"code": "no-official", "version": version}
                return
            want = off["sha"]
        if not KEY.is_file():
            job.state, job.error = "error", {"code": "denied"}
            return
        job.step = "connect"
        script = (HERE / "deck.sh").read_bytes().replace(b"\r\n", b"\n")
        unit = "az-hebrew-launcher-%d" % int(time.time())
        start = ("cat > /tmp/az-hebrew-launcher.sh && : > /tmp/az-hebrew-launcher.log && "
                 "systemd-run --unit=%s sh /tmp/az-hebrew-launcher.sh %s %s" % (unit, mode, want))
        r = run(ssh_cmd(ip, start), timeout=40, data=script)
        err = r.stderr.decode("utf-8", "replace")
        if r.returncode:
            job.say(err)
            job.state, job.error = "error", {"code": ssh_error_kind(err), "text": err.strip()[-300:]}
            return
        job.say("started on the deck: %s %s" % (unit, mode))
        deadline, seen = time.time() + 300, 0
        while time.time() < deadline:
            time.sleep(2)
            try:
                r = run(ssh_cmd(ip, "cat /tmp/az-hebrew-launcher.log"), timeout=25)
            except subprocess.TimeoutExpired:
                continue  # the deck's SSH restarts while the loader runs its hooks
            if r.returncode:
                continue
            lines = r.stdout.decode("utf-8", "replace").splitlines()
            for line in lines[seen:]:
                job.say(line)
                m = re.match(r"@@ (\S+) ?(.*)", line.strip())
                if not m:
                    continue
                key, rest = m.group(1), m.group(2).split()
                if key == "step" and rest:
                    job.step = rest[0]
                elif key == "error":
                    job.state, job.error = "error", {"code": "deck-" + (rest[0] if rest else "?")}
                    return
                elif key == "done":
                    job.result = {"mode": mode, "status": rest}
                    if mode == "apply":
                        set_setting(deck_done=rest[0] if rest else None)
                    else:
                        set_setting(deck_done=None)
                    return
            seen = len(lines)
        job.state, job.error = "error", {"code": "deck-timeout"}

    def forget_host(self, ip):
        if KNOWN.is_file():
            run([openssh("ssh-keygen"), "-R", ip, "-f", str(KNOWN)])


# ---------------------------------------------------------------- glyphs for the page
def glyph_paths():
    """heb_glyphs.h as SVG paths (font units, y up): what the deck will draw."""
    src = (MOD / "azmods/files/src/azmod/heb_glyphs.h").read_text(encoding="utf-8")
    upm = int(re.search(r"HEB_DONOR_UPM (\d+)", src).group(1))
    rows = re.findall(r"\{(0x[0-9a-f]+), (\d+), (\d+), (\d+), (\d+), (\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+)\}", src)
    body = src.split("HEB_PTS")[1].split("};")[0]
    pts = [(int(a), int(b)) for a, b in re.findall(r"\{(-?\d+), (-?\d+)\}", body)]
    tags = [int(t) for t in re.findall(r"\d+", src.split("HEB_TAGS")[1].split("= {")[1].split("};")[0])]
    ends = [int(t) for t in re.findall(r"\d+", src.split("HEB_ENDS")[1].split("= {")[1].split("};")[0])]
    out = {}
    for r in rows:
        cp, adv, p0, n, c0, nc = (int(v, 0) for v in r[:6])
        d, start = [], 0
        for e in ends[c0:c0 + nc]:
            seq = [(pts[p0 + i], tags[p0 + i]) for i in range(start, e + 1)]
            start = e + 1
            if not seq:
                continue
            # start on an on-curve point (or the midpoint of two control points)
            k = next((i for i, (_, on) in enumerate(seq) if on), None)
            if k is None:
                a, b = seq[0][0], seq[-1][0]
                seq = [(((a[0] + b[0]) / 2, (a[1] + b[1]) / 2), 1)] + seq
                k = 0
            seq = seq[k:] + seq[:k]
            seq.append(seq[0])
            d.append("M%g %g" % seq[0][0])
            i = 1
            while i < len(seq):
                (x, y), on = seq[i]
                if on:
                    d.append("L%g %g" % (x, y))
                    i += 1
                    continue
                (nx, ny), non = seq[i + 1] if i + 1 < len(seq) else seq[0]
                if non:
                    d.append("Q%g %g %g %g" % (x, y, nx, ny))
                    i += 2
                else:
                    d.append("Q%g %g %g %g" % (x, y, (x + nx) / 2, (y + ny) / 2))
                    i += 1
            d.append("Z")
        out[cp] = {"adv": adv, "d": "".join(d)}
    return {"upm": upm, "glyphs": out}


# ---------------------------------------------------------------- HTTP
STATIC = {"/": ("index.html", "text/html; charset=utf-8"), "/style.css": ("style.css", "text/css; charset=utf-8"),
          "/app.js": ("app.js", "text/javascript; charset=utf-8")}


class Handler(BaseHTTPRequestHandler):
    server_version = "AZHebrew"
    sys_version = ""

    def log_message(self, *a):
        pass

    def reply(self, code, body, mime="application/json; charset=utf-8"):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self'; script-src 'self'; "
                         "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(body)

    def ok(self, auth=True):
        app = self.server.app
        if self.headers.get("Host") != self.server.authority:
            self.reply(403, {"error": "host"})
            return False
        if auth and not secrets.compare_digest(self.headers.get("Authorization", ""), "Bearer " + app.token):
            self.reply(403, {"error": "token"})
            return False
        app.last_seen = time.time()
        return True

    def do_GET(self):
        app = self.server.app
        if self.path in STATIC:
            if self.ok(auth=False):
                name, mime = STATIC[self.path]
                self.reply(200, (WEB / name).read_bytes(), mime)
        elif self.path == "/api/state":
            if self.ok():
                self.reply(200, app.state())
        elif self.path == "/api/glyphs":
            if self.ok():
                self.reply(200, self.server.glyphs)
        elif self.path == "/api/ping":
            if self.ok():
                self.reply(200, {"ok": True})
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        if not self.ok():
            return
        app = self.server.app
        try:
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}") if n < 65536 else {}
        except ValueError:
            body = {}
        letter = str(body.get("stick", ""))[:1].upper()
        ip = str(body.get("ip", "")).strip()
        p = self.path
        started = None
        if p == "/api/build":
            started = app.start("build", app.do_build)
        elif p == "/api/install" and letter.isalpha():
            started = app.start("install", app.do_install, letter)
        elif p == "/api/find":
            started = app.start("find", app.find_deck)
        elif p == "/api/apply":
            started = app.start("apply", app.deck_run, ip, "apply")
        elif p == "/api/restore" and letter.isalpha():
            started = app.start("restore-stick", app.do_restore_stick, letter)
        elif p == "/api/restore-deck":
            started = app.start("restore-deck", app.deck_run, ip, "restore")
        elif p == "/api/forget-host":
            app.forget_host(ip)
            started = True
        elif p == "/api/settings":
            if "deck_ip" in body:
                set_setting(deck_ip=ip)
            if letter.isalpha():
                set_setting(stick=letter)
            started = True
        elif p == "/api/open-app":
            exe = azmods_app()
            if exe:
                subprocess.Popen([str(exe)], creationflags=NOWIN)
            started = bool(exe)
        elif p == "/api/open-folder":
            if os.name == "nt":
                os.startfile(str(DATA))
            started = True
        elif p == "/api/quit":
            self.reply(200, {"ok": True})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        if started is None:
            self.reply(400, {"error": "bad request"})
        else:
            self.reply(200 if started else 409, {"ok": bool(started)})


def open_window(url):
    if os.name == "nt":
        for var, rel in (("PROGRAMFILES(X86)", r"Microsoft\Edge\Application\msedge.exe"),
                         ("PROGRAMFILES", r"Microsoft\Edge\Application\msedge.exe"),
                         ("PROGRAMFILES", r"Google\Chrome\Application\chrome.exe"),
                         ("LOCALAPPDATA", r"Google\Chrome\Application\chrome.exe")):
            exe = Path(os.environ.get(var, "")) / rel
            if exe.is_file():
                try:
                    subprocess.Popen([str(exe), "--app=" + url, "--window-size=1180,900"], creationflags=NOWIN)
                    return
                except OSError:
                    pass
    import webbrowser
    webbrowser.open(url)


def already_running():
    """Another launcher answers: show its window and say so."""
    info = load_json(DATA / "server.json", None)
    if not info:
        return False
    try:
        c = http.client.HTTPConnection("127.0.0.1", info["port"], timeout=1.5)
        c.request("GET", "/api/ping", headers={"Authorization": "Bearer " + info["token"],
                                                "Host": "127.0.0.1:%d" % info["port"]})
        if c.getresponse().status == 200:
            open_window("http://127.0.0.1:%d/#%s" % (info["port"], info["token"]))
            return True
    except (OSError, KeyError, http.client.HTTPException):
        pass
    return False


def main():
    DATA.mkdir(parents=True, exist_ok=True)
    if already_running():
        return
    app = App()
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    srv.daemon_threads = True
    srv.app = app
    port = srv.server_address[1]
    srv.authority = "127.0.0.1:%d" % port
    srv.glyphs = glyph_paths()
    save_json(DATA / "server.json", {"port": port, "token": app.token, "pid": os.getpid()})
    log("launcher started on port %d (mod %s, AZ-MODS %s)" % (port, MOD, source_version()))

    def idle():
        while True:
            time.sleep(30)
            busy = app.job and app.job.state == "running"
            if not busy and time.time() - app.last_seen > IDLE_EXIT:
                srv.shutdown()
                return
    threading.Thread(target=idle, daemon=True).start()
    if not os.environ.get("AZH_NO_WINDOW"):  # the tests drive the page themselves
        open_window("http://127.0.0.1:%d/#%s" % (port, app.token))
    try:
        srv.serve_forever(poll_interval=0.5)
    finally:
        try:
            (DATA / "server.json").unlink()
        except OSError:
            pass
        log("launcher stopped")


if __name__ == "__main__":
    main()
