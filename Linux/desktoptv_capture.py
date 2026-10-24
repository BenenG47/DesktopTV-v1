#!/usr/bin/env python3
# DesktopTV Linux helper - everything the DesktopTV ReShade add-on does on Windows that Wine cannot do
# from inside the game (KDE Plasma, Wayland):
#
#   picture  a monitor (or a virtual monitor / a window) through the desktop portal; desktoptv_pw sends it
#            and the mouse cursor (as separate metadata, like Desktop Duplication) to the add-on over TCP
#   PC mode  puts the real mouse pointer on that monitor (portal "remote desktop" pointer), gives the
#            window under it the keyboard, keeps the pointer there, catches the PC mode / hide keys with
#            KDE global shortcuts, keeps the game in front, and brings the pointer back into the game
#   sound    mutes / fades / pans the programs whose windows are on that monitor (PipeWire, via pactl),
#            following the add-on's rules and Jodi's position; always gives the sound back
#
# The add-on connects to 127.0.0.1:<port> (ReShade.ini [DesktopTV] LinuxPort, default 47474).
# First start: KDE asks what to share and to allow pointer control. The choice is remembered in
# ~/.config/desktoptv/ (--choose asks again).
#
# Needs: python-gobject, python-dbus, pipewire-pulse (pactl), KWin (Plasma 6), and desktoptv_pw
# (built by build_linux.sh next to this file in build/).

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib, GLibUnix

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_DIR = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "desktoptv")
TOKEN_FILE = os.path.join(CONFIG_DIR, "restore_token_rd")       # remote desktop + screen cast
TOKEN_FILE_SC = os.path.join(CONFIG_DIR, "restore_token")       # screen cast only (--no-control)
TOUCHED_FILE = os.path.join(CONFIG_DIR, "touched.json")         # sound we changed (survives a crash)
PORTAL = "org.freedesktop.portal.Desktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
SCREENCAST = "org.freedesktop.portal.ScreenCast"
REMOTEDESKTOP = "org.freedesktop.portal.RemoteDesktop"
BUS_NAME = "org.desktoptv.Helper"
OBJ_PATH = "/helper"
IFACE = "org.desktoptv.Helper"


def log(msg):
    print("DesktopTV: " + msg, flush=True)


# ---------------------------------------------------------------- Windows key codes -> Qt key codes

def vk_to_qt(vk):
    special = {
        0x2D: 0x01000006,  # Insert
        0x2E: 0x01000007,  # Delete
        0x13: 0x01000008,  # Pause
        0x24: 0x01000010,  # Home
        0x23: 0x01000011,  # End
        0x21: 0x01000016,  # Page Up
        0x22: 0x01000017,  # Page Down
        0x91: 0x01000026,  # Scroll Lock
        0x90: 0x01000025,  # Num Lock
        0x2C: 0x01000009,  # Print
    }
    if vk in special:
        return special[vk]
    if 0x70 <= vk <= 0x87:  # F1..F24
        return 0x01000030 + (vk - 0x70)
    if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:  # digits, letters
        return vk
    return 0


# ---------------------------------------------------------------- KWin (window list, focus, pointer fence)

WATCH_JS = r"""
function info(w) {
    var g = w.frameGeometry, c = w.clientGeometry;
    return ["win", w.internalId.toString(), w.pid, w.output ? w.output.name : "", w.minimized ? 1 : 0,
            w.normalWindow ? 1 : 0, w.desktopWindow ? 1 : 0, g.x, g.y, g.width, g.height,
            c.x, c.y, c.width, c.height, w.resourceClass, w.keepAbove ? 1 : 0].join("\t");
}
function snapshot() {
    var out = [];
    var s = workspace.screens;
    for (var i = 0; i < s.length; ++i) {
        var g = s[i].geometry;
        out.push(["output", s[i].name, g.x, g.y, g.width, g.height].join("\t"));
    }
    var ws = workspace.windowList();
    for (var i = 0; i < ws.length; ++i) out.push(info(ws[i]));
    var a = workspace.activeWindow;
    out.push("active\t" + (a ? a.internalId.toString() : ""));
    callDBus("@BUS@", "@PATH@", "@IFACE@", "Snapshot", out.join("\n"));
}
function watch(w) {
    w.outputChanged.connect(snapshot);
    w.minimizedChanged.connect(snapshot);
}
workspace.windowList().forEach(watch);
workspace.windowAdded.connect(function (w) { watch(w); snapshot(); });
workspace.windowRemoved.connect(snapshot);
workspace.windowActivated.connect(snapshot);
workspace.screensChanged.connect(snapshot);
snapshot();
"""

# Loaded while PC mode is on: keeps the game in front, gives the keyboard to the window under the
# pointer, and reports when the pointer leaves the shown monitor (the helper pushes it back)
PCMODE_JS = r"""
var P = @PARAMS@;
var ws = workspace.windowList(), game = null;
for (var i = 0; i < ws.length; ++i) if (ws[i].internalId.toString() == P.game) game = ws[i];
if (game && P.keep) game.keepAbove = true;
var st = workspace.stackingOrder, pick = null;
for (var i = st.length - 1; i >= 0 && !pick; --i) {
    var w = st[i];
    if (w === game || w.minimized || !(w.normalWindow || w.dialog)) continue;
    var g = w.frameGeometry;
    if (P.px >= g.x && P.px < g.x + g.width && P.py >= g.y && P.py < g.y + g.height) pick = w;
}
for (var i = 0; i < ws.length && !pick; ++i)
    if (ws[i].desktopWindow && ws[i].output && P.outs.indexOf(ws[i].output.name) >= 0 &&
        P.px >= ws[i].frameGeometry.x && P.px < ws[i].frameGeometry.x + ws[i].frameGeometry.width &&
        P.py >= ws[i].frameGeometry.y && P.py < ws[i].frameGeometry.y + ws[i].frameGeometry.height) pick = ws[i];
if (pick) workspace.activeWindow = pick;
workspace.cursorPosChanged.connect(function () {
    var c = workspace.cursorPos;
    if (c.x < P.x || c.y < P.y || c.x >= P.x + P.w || c.y >= P.y + P.h)
        callDBus("@BUS@", "@PATH@", "@IFACE@", "Outside", c.x, c.y);
});
"""

# One shot when PC mode ends: let go of the game's keep-above, optionally give it the keyboard
PCOFF_JS = r"""
var P = @PARAMS@;
var ws = workspace.windowList();
for (var i = 0; i < ws.length; ++i) {
    var w = ws[i];
    if (w.internalId.toString() != P.game) continue;
    if (P.keep) w.keepAbove = P.was_above;
    if (P.focus) workspace.activeWindow = w;
}
"""


class KWin:
    def __init__(self, bus):
        self.bus = bus
        self.scripting = dbus.Interface(bus.get_object("org.kde.KWin", "/Scripting"), "org.kde.kwin.Scripting")
        self.dir = tempfile.mkdtemp(prefix="desktoptv-")
        self.counter = 0
        self.loaded = set()

    def _fill(self, js, params=None):
        js = js.replace("@BUS@", BUS_NAME).replace("@PATH@", OBJ_PATH).replace("@IFACE@", IFACE)
        if params is not None:
            js = js.replace("@PARAMS@", json.dumps(params))
        return js

    def load(self, name, js, params=None):
        self.unload(name)
        path = os.path.join(self.dir, f"{name}.js")
        with open(path, "w") as f:
            f.write(self._fill(js, params))
        sid = int(self.scripting.loadScript(path, name, signature="ss"))
        if sid < 0:
            log(f"KWin refused script {name}")
            return False
        dbus.Interface(self.bus.get_object("org.kde.KWin", f"/Scripting/Script{sid}"), "org.kde.kwin.Script").run()
        self.loaded.add(name)
        return True

    def once(self, js, params=None):
        self.counter += 1
        name = f"desktoptv_once{self.counter}"
        if self.load(name, js, params):
            GLib.timeout_add(2000, lambda: (self.unload(name), False)[1])

    def unload(self, name):
        try:
            self.scripting.unloadScript(name)
        except dbus.DBusException:
            pass
        self.loaded.discard(name)

    def close(self):
        for name in list(self.loaded):
            self.unload(name)


# ---------------------------------------------------------------- KDE global shortcuts (PC mode keys)

class GlobalKeys:
    """Grabs the PC mode / hide keys system-wide while PC mode is on (the game does not see them then)."""

    def __init__(self, bus, on_key):
        self.kga = dbus.Interface(bus.get_object("org.kde.kglobalaccel", "/kglobalaccel"), "org.kde.KGlobalAccel")
        self.on_key = on_key
        self.actions = {}
        self.matched = False
        self.bus = bus

    def _action(self, name):
        return dbus.Array(["desktoptv", name, "DesktopTV", f"DesktopTV {name}"], signature="s")

    def grab(self, keys):  # keys: {"pc": qt_key, "hide": qt_key}
        self.release()
        for name, qt in keys.items():
            if not qt:
                continue
            a = self._action(name)
            self.kga.doRegister(a)
            got = self.kga.setShortcut(a, dbus.Array([qt], signature="i"), dbus.UInt32(6))  # SetPresent | NoAutoloading
            if list(got) != [qt]:
                log(f"the {name} key is taken by another KDE shortcut - it only works while the game has focus")
            self.actions[name] = a
        if self.actions and not self.matched:
            path = self.kga.getComponent("desktoptv")
            self.bus.add_signal_receiver(lambda comp, act, ts: self.on_key(str(act)), "globalShortcutPressed",
                                         "org.kde.kglobalaccel.Component", None, path)
            self.matched = True

    def release(self):
        for a in self.actions.values():
            try:
                self.kga.unRegister(a)
            except dbus.DBusException:
                pass
        self.actions = {}


# ---------------------------------------------------------------- sound (programs on the shown monitor)

def pactl(*args):
    try:
        return subprocess.run(["pactl", *args], capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def parent_pids(pid, depth=1):
    out = []
    for _ in range(depth):
        try:
            with open(f"/proc/{pid}/stat") as f:
                stat = f.read()
            pid = int(stat[stat.rfind(")") + 2:].split()[1])
        except (OSError, ValueError, IndexError):
            break
        if pid <= 1:
            break
        out.append(pid)
    return out


class Sound:
    """Port of the add-on's Windows sound rules (update_sound) to PipeWire streams."""

    def __init__(self):
        self.streams = {}   # sink-input index -> state
        self.touched = self._load_touched()

    # ---- crash record: binary -> {"volumes": [...], "muted": bool}
    def _load_touched(self):
        try:
            with open(TOUCHED_FILE) as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def _save_touched(self):
        os.makedirs(CONFIG_DIR, exist_ok=True)
        if not self.touched:
            try:
                os.remove(TOUCHED_FILE)
            except OSError:
                pass
            return
        with open(TOUCHED_FILE, "w") as f:
            json.dump(self.touched, f)

    def _note(self, binary, volumes, muted):
        if not binary:
            return
        rec = self.touched.get(binary)
        if rec is None:
            self.touched[binary] = {"volumes": volumes, "muted": muted}
            self._save_touched()
        elif muted and not rec["muted"]:
            rec["muted"] = True
            self._save_touched()

    @staticmethod
    def _list():
        try:
            return json.loads(pactl("-f", "json", "list", "sink-inputs") or "[]")
        except ValueError:
            return []

    @staticmethod
    def _volumes(si):
        return [int(ch["value"]) for ch in si.get("volume", {}).values()]

    def heal(self):
        """Programs left changed by a run that ended without giving the sound back."""
        if not self.touched:
            return
        for si in self._list():
            binary = si.get("properties", {}).get("application.process.binary", "")
            rec = self.touched.get(binary)
            if rec is None:
                continue
            if rec.get("muted"):
                pactl("set-sink-input-mute", str(si["index"]), "0")
            vols = rec.get("volumes") or []
            if len(vols) == len(self._volumes(si)):
                pactl("set-sink-input-volume", str(si["index"]), *map(str, vols))
            log(f"sound: {binary} given back its sound (left changed by an earlier run)")
        self.touched = {}
        self._save_touched()

    def update(self, places, want, gl, gr, restore_all=False):
        """places: pid -> 1 (windows only on the shown monitor) / 2 (also elsewhere)."""
        spatial = gl >= 0
        seen = set()
        for si in self._list():
            idx = si["index"]
            props = si.get("properties", {})
            try:
                pid = int(props.get("application.process.id", 0))
            except ValueError:
                pid = 0
            if pid == 0 or pid == os.getpid() or props.get("application.process.binary", "").startswith("wine"):
                continue  # not our own sound, nor the game's (it runs in Wine)
            seen.add(idx)
            st = self.streams.get(idx)
            if restore_all:
                if st:
                    self._restore(idx, st)
                continue
            f = places.get(pid, 0)
            if f == 0:
                for p in parent_pids(pid):
                    if p in places:
                        f = places[p]
                        break
            binary = props.get("application.process.binary", "")
            if f != 1:
                if st:
                    self._restore(idx, st)
                    if st.get("muted_by_us"):
                        log(f"sound: unmuted {binary} (left the second screen)")
                    del self.streams[idx]
                continue
            if st is None:
                st = self.streams[idx] = {"binary": binary, "orig": self._volumes(si), "muted_by_us": False,
                                          "cur": None, "applied": None}
            muted = bool(si.get("mute"))
            if spatial:
                if st["muted_by_us"] or (muted and st["cur"] is None and not st.get("was_muted_checked")):
                    pactl("set-sink-input-mute", str(idx), "0")
                    st["muted_by_us"] = False
                st["was_muted_checked"] = True
                if st["cur"] is None:
                    st["cur"] = [gl, gr]
                    log(f"sound: {binary} follows Jodi's position")
                st["cur"][0] += max(-0.15, min(0.15, gl - st["cur"][0]))
                st["cur"][1] += max(-0.15, min(0.15, gr - st["cur"][1]))
                self._note(binary, st["orig"], False)
                self._apply_gains(idx, st)
            else:
                if st["applied"] is not None:
                    self._restore_volume(idx, st)
                if not want and not muted:
                    self._note(binary, st["orig"], True)
                    pactl("set-sink-input-mute", str(idx), "1")
                    st["muted_by_us"] = True
                    log(f"sound: muted {binary} (on the second screen)")
                elif want and muted:
                    pactl("set-sink-input-mute", str(idx), "0")
                    st["muted_by_us"] = False
                    log(f"sound: unmuted {binary}")
        for idx in list(self.streams):
            if idx not in seen:
                del self.streams[idx]
        if restore_all:
            self.streams = {}
            self.touched = {}
            self._save_touched()

    def _apply_gains(self, idx, st):
        l, r = st["cur"]
        orig = st["orig"]
        if len(orig) == 2:
            vols = [int(orig[0] * l), int(orig[1] * r)]
        else:
            vols = [int(v * max(l, r)) for v in orig]  # not stereo: fade only
        if vols != st["applied"]:
            pactl("set-sink-input-volume", str(idx), *map(str, vols))
            st["applied"] = vols

    def _restore_volume(self, idx, st):
        if st["applied"] is not None:
            pactl("set-sink-input-volume", str(idx), *map(str, st["orig"]))
            st["applied"] = None
        st["cur"] = None

    def _restore(self, idx, st):
        self._restore_volume(idx, st)
        if st.get("muted_by_us"):
            pactl("set-sink-input-mute", str(idx), "0")
            st["muted_by_us"] = False


# ---------------------------------------------------------------- portal

class PortalSession:
    """One desktop-portal session. KDE asks for remote control without a choice of screen (it always gives
    the whole workspace), so the picture and the pointer use two sessions:
      picture: screen cast of the chosen monitor / virtual monitor / window, cursor as metadata
      control: remote desktop (pointer); its whole-workspace stream is only the coordinate space for moving
               the pointer anywhere on the desktop
    on_ready(session, node, props) is called when the stream exists."""

    counter = 0

    def __init__(self, bus, control, token_file, on_ready, on_fail):
        self.bus = bus
        self.portal = bus.get_object(PORTAL, PORTAL_PATH)
        self.sc = dbus.Interface(self.portal, SCREENCAST)
        self.rd = dbus.Interface(self.portal, REMOTEDESKTOP)
        self.control = control
        self.token_file = token_file
        self.on_ready = on_ready
        self.on_fail = on_fail
        self.session = None
        self.sender = bus.get_unique_name()[1:].replace(".", "_")

    def _request(self, iface, method, *args, then):
        PortalSession.counter += 1
        token = f"desktoptv{os.getpid()}_{PortalSession.counter}"
        path = f"/org/freedesktop/portal/desktop/request/{self.sender}/{token}"

        def done(response, results):
            match.remove()
            if response != 0:
                log(f"{method} was cancelled or failed (response {response})")
                self.on_fail()
                return
            then(results)

        match = self.bus.add_signal_receiver(done, "Response", "org.freedesktop.portal.Request", PORTAL, path)
        args[-1]["handle_token"] = token
        getattr(iface, method)(*args)

    def _prop(self, iface, name):
        return self.portal.Get(iface, name, dbus_interface="org.freedesktop.DBus.Properties")

    def _token(self, opts):
        opts["persist_mode"] = dbus.UInt32(2)
        try:
            with open(self.token_file) as f:
                opts["restore_token"] = f.read().strip()
            return True
        except OSError:
            return False

    def start(self):
        iface = self.rd if self.control else self.sc
        PortalSession.counter += 1
        self._request(iface, "CreateSession", {"session_handle_token": f"desktoptv{os.getpid()}s{PortalSession.counter}"}, then=self._created)

    def _created(self, results):
        self.session = results["session_handle"]
        if self.control:
            opts = {"types": dbus.UInt32(2)}  # pointer
            if not self._token(opts):
                log("KDE will ask to allow pointer control (for PC mode)")
            self._request(self.rd, "SelectDevices", self.session, opts, then=self._select_sources)
        else:
            self._select_sources({})

    def _select_sources(self, _results):
        if self.control:
            opts = {"types": dbus.UInt32(1), "multiple": False, "cursor_mode": dbus.UInt32(1)}
        else:
            modes = int(self._prop(SCREENCAST, "AvailableCursorModes"))
            types = int(self._prop(SCREENCAST, "AvailableSourceTypes")) & 7  # monitor, window, virtual monitor
            opts = {"types": dbus.UInt32(types or 1), "multiple": False}
            if modes & 4:
                opts["cursor_mode"] = dbus.UInt32(4)  # cursor as metadata: the add-on draws it (PC mode only)
            elif modes & 1:
                opts["cursor_mode"] = dbus.UInt32(1)
            if not self._token(opts):
                log("KDE will ask which monitor (or virtual monitor / window) to share")
        self._request(self.sc, "SelectSources", self.session, opts, then=self._selected)

    def _selected(self, _results):
        iface = self.rd if self.control else self.sc
        self._request(iface, "Start", self.session, "", {}, then=self._started)

    def _started(self, results):
        token = results.get("restore_token")
        if token:
            os.makedirs(CONFIG_DIR, exist_ok=True)
            with open(self.token_file, "w") as f:
                f.write(str(token))
        if self.control and not (int(results.get("devices", 0)) & 2):
            log("pointer control was not allowed - PC mode cannot move the mouse (start with --choose to ask again)")
            self.on_fail()
            return
        streams = results.get("streams", [])
        if not streams:
            log("the portal returned no stream")
            self.on_fail()
            return
        node, props = streams[0]
        self.on_ready(self, int(node), {str(k): v for k, v in props.items()})

    def open_pipewire(self):
        return self.sc.OpenPipeWireRemote(self.session, {}).take()

    def pointer_to(self, node, x, y):
        self.rd.NotifyPointerMotionAbsolute(self.session, {}, dbus.UInt32(node), float(x), float(y))


# ---------------------------------------------------------------- the helper

class Helper(dbus.service.Object):
    def __init__(self, bus, args, loop):
        self.bus_name = dbus.service.BusName(BUS_NAME, bus)
        super().__init__(bus, OBJ_PATH)
        self.bus = bus
        self.args = args
        self.loop = loop
        self.kwin = KWin(bus)
        self.keys = GlobalKeys(bus, self._global_key)
        self.sound = Sound()
        self.picture = PortalSession(bus, False, TOKEN_FILE_SC, self._portal_ready, self._quit)
        self.control = None         # remote desktop session for the pointer (None = no PC mode)
        self.ctl_node = 0
        self.ctl_size = None        # logical size / position of the workspace stream of that session
        self.ctl_pos = None
        self.child = None
        self.node = 0
        self.stream_size = None     # logical size of the shared monitor / window
        self.stream_pos = None      # logical position (monitors), None for a window
        self.source_type = 0
        self.buf_size = None        # pixel size of the stream's pictures
        self.outputs = {}           # name -> (x, y, w, h)
        self.windows = {}           # id -> dict
        self.active = ""
        self.cursor = None          # (x, y) logical, inside the stream; None when not on it
        self.addon = False
        self.qt_keys = {"pc": vk_to_qt(0x2D), "hide": vk_to_qt(0x91)}
        self.keepfront = True
        self.pc = None              # PC mode state
        self.last_outside = 0.0

    # ---- start / stop
    def start(self):
        self.sound.heal()
        self.kwin.load("desktoptv_watch", WATCH_JS)
        self.picture.start()

    def _quit(self, *_):
        self.loop.quit()

    def shutdown(self):
        if self.pc:
            self._pc_off(focus_game=False, release=True)
        self.sound.update({}, True, -1, -1, restore_all=True)
        self.keys.release()
        self.kwin.close()
        if self.child and self.child.poll() is None:
            try:
                self.child.stdin.write("quit\n")
                self.child.stdin.flush()
                self.child.wait(2)
            except (OSError, subprocess.SubprocessError):
                self.child.kill()

    def _control_ready(self, session, node, props):
        size, pos = props.get("size"), props.get("position")
        self.ctl_node = node
        self.ctl_size = (int(size[0]), int(size[1])) if size else None
        self.ctl_pos = (int(pos[0]), int(pos[1])) if pos else None
        self.control = session
        log("pointer control allowed (PC mode available)")

    def _control_failed(self):
        self.control = None

    def pointer_to(self, gx, gy):
        """Move the real pointer to a point of the desktop (logical coordinates)."""
        if self.control is None:
            return
        if self.ctl_pos is not None:
            x0, y0 = self.ctl_pos
        elif self.outputs:  # the whole workspace: it starts at the top-left of all monitors
            x0 = min(o[0] for o in self.outputs.values())
            y0 = min(o[1] for o in self.outputs.values())
        else:
            x0 = y0 = 0
        self.control.pointer_to(self.ctl_node, gx - x0, gy - y0)

    def _portal_ready(self, session, node, props):
        fd = session.open_pipewire()
        if not self.args.no_control:  # second dialog: pointer control
            PortalSession(self.bus, True, TOKEN_FILE, self._control_ready, self._control_failed).start()
        self.node = node
        size, pos = props.get("size"), props.get("position")
        self.stream_size = (int(size[0]), int(size[1])) if size else None
        self.stream_pos = (int(pos[0]), int(pos[1])) if pos else None
        self.source_type = int(props.get("source_type", 0))
        kind = {1: "monitor", 2: "window", 4: "virtual monitor"}.get(self.source_type, "source")
        log(f"sharing a {kind} (PipeWire node {node}" + (f", {self.stream_size[0]}x{self.stream_size[1]}" if self.stream_size else "") + ")")
        if self.source_type == 2:
            log("a single window is shared: PC mode and the second-screen sound need a monitor (--choose)")
        exe = os.path.join(HERE, "build", "desktoptv_pw")
        cmd = [exe, "--fd", str(fd), "--node", str(node), "--port", str(self.args.port), "--fps", str(self.args.fps)]
        if self.args.size:
            cmd += ["--size", self.args.size]
        try:
            self.child = subprocess.Popen(cmd, pass_fds=(fd,), stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        except OSError as e:
            log(f"cannot start {exe} ({e}) - run build_linux.sh first")
            self._quit()
            return
        os.close(fd)
        GLib.io_add_watch(self.child.stdout, GLib.PRIORITY_DEFAULT, GLib.IO_IN | GLib.IO_HUP, self._child_output)

    def _send(self, text):
        if self.child and self.child.poll() is None:
            try:
                self.child.stdin.write("send " + text + "\n")
                self.child.stdin.flush()
            except OSError:
                pass

    # ---- the shown monitor in desktop coordinates
    def _shown_rect(self):
        if self.stream_size is None or self.source_type == 2:
            return None
        if self.stream_pos is not None:
            return (*self.stream_pos, *self.stream_size)
        # No position from the portal (KDE's "full workspace"): find it by size
        if self.outputs:
            g = list(self.outputs.values())
            x0, y0 = min(o[0] for o in g), min(o[1] for o in g)
            x1, y1 = max(o[0] + o[2] for o in g), max(o[1] + o[3] for o in g)
            if (x1 - x0, y1 - y0) == self.stream_size:
                return (x0, y0, *self.stream_size)
            same = [o for o in g if (o[2], o[3]) == self.stream_size]
            if len(same) == 1:
                return same[0]
        return None

    def _shown_outputs(self):
        """Names of the monitors inside the shared area."""
        r = self._shown_rect()
        if r is None:
            return set()
        return {name for name, g in self.outputs.items()
                if g[0] >= r[0] and g[1] >= r[1] and g[0] + g[2] <= r[0] + r[2] and g[1] + g[3] <= r[1] + r[3]}

    # ---- output of desktoptv_pw
    def _child_output(self, src, cond):
        line = src.readline()
        if not line:
            log("picture program ended")
            self._quit()
            return False
        line = line.rstrip("\n")
        if line.startswith("addon "):
            self._addon_message(line[6:])
        elif line.startswith("cursor "):
            x, y, vis = (int(v) for v in line.split()[1:4])
            if self.buf_size and self.stream_size:
                kx, ky = self.stream_size[0] / self.buf_size[0], self.stream_size[1] / self.buf_size[1]
                inside = 0 <= x < self.buf_size[0] and 0 <= y < self.buf_size[1] and vis
                self.cursor = (x * kx, y * ky) if inside else None
        elif line.startswith("stream "):
            self.buf_size = tuple(int(v) for v in line.split()[1:3])
        elif line == "client 1":
            self.addon = True
            log("add-on connected")
            self.sound.heal()
        elif line == "client 0":
            self.addon = False
            log("add-on disconnected (game closed?) - giving everything back")
            if self.pc:
                self._pc_off(focus_game=False, release=False)
            self.sound.update({}, True, -1, -1, restore_all=True)
        elif line == "ended":
            log("screen sharing ended")
            self._quit()
            return False
        return True

    # ---- messages from the add-on
    def _addon_message(self, msg):
        parts = msg.split()
        if not parts:
            return
        cmd = parts[0]
        if cmd == "hello":
            opts = dict(p.split("=", 1) for p in parts[2:] if "=" in p)
            self.qt_keys = {"pc": vk_to_qt(int(opts.get("pckey", 45))), "hide": vk_to_qt(int(opts.get("hidekey", 145)))}
            self.keepfront = opts.get("keepfront", "1") != "0"
            log(f"add-on {parts[1] if len(parts) > 1 else ''} says hello")
            self._send("info helper ready: " + ("pointer control on" if self.control else "no pointer control (PC mode unavailable)")
                       + (", shared " + {1: "monitor", 2: "window", 4: "virtual monitor"}.get(self.source_type, "?")))
        elif cmd == "pc_on":
            self._pc_on(float(parts[1]), float(parts[2]))
        elif cmd == "pc_off":
            if parts[1] == "game":
                self._pc_off(focus_game=True, release=False, target=(float(parts[2]), float(parts[3])))
            else:
                self._pc_off(focus_game=False, release=True)
        elif cmd == "sound":
            self.sound.update(self._places(), parts[1] == "1", float(parts[2]), float(parts[3]))
        elif cmd == "sound_restore":
            self.sound.update({}, True, -1, -1, restore_all=True)
            self._send("restored")

    # ---- window list from KWin
    @dbus.service.method(IFACE, in_signature="s")
    def Snapshot(self, text):
        outputs, windows, active = {}, {}, ""
        for line in str(text).split("\n"):
            f = line.split("\t")
            if f[0] == "output" and len(f) >= 6:
                outputs[f[1]] = tuple(int(float(v)) for v in f[2:6])
            elif f[0] == "win" and len(f) >= 16:
                windows[f[1]] = {"pid": int(f[2]), "output": f[3], "minimized": f[4] == "1", "normal": f[5] == "1",
                                 "desktop": f[6] == "1", "frame": tuple(float(v) for v in f[7:11]),
                                 "client": tuple(float(v) for v in f[11:15]), "class": f[15],
                                 "above": len(f) > 16 and f[16] == "1"}
            elif f[0] == "active":
                active = f[1] if len(f) > 1 else ""
        self.outputs, self.windows = outputs, windows
        if active != self.active:
            self.active = active
            self._check_focus()

    @dbus.service.method(IFACE, in_signature="ii")
    def Outside(self, x, y):
        # PC mode: the pointer left the shown monitor - put it back on its edge (like ClipCursor)
        if not self.pc:
            return
        r = self._shown_rect()
        if r is None:
            return
        self.pointer_to(min(max(x, r[0]), r[0] + r[2] - 1), min(max(y, r[1]), r[1] + r[3] - 1))

    def _places(self):
        """pid -> 1 if all its visible windows are on the shown monitor, 2 if some are elsewhere."""
        shown = self._shown_outputs()
        places = {}
        if not shown:
            return places
        for w in self.windows.values():
            if w["minimized"] or not w["normal"] or w["frame"][2] < 50 or w["frame"][3] < 50:
                continue
            places[w["pid"]] = places.get(w["pid"], 0) | (1 if w["output"] in shown else 2)
        return places

    # ---- PC mode
    def _pc_on(self, u, v):
        if self.pc:
            return
        r = self._shown_rect()
        if self.control is None or r is None:
            self._send("pc_lost " + ("no pointer control (portal)" if self.control is None else "a monitor must be shared for PC mode"))
            return
        x = u * (r[2] - 1) if u >= 0 else r[2] / 2
        y = v * (r[3] - 1) if v >= 0 else r[3] / 2
        game = self.active
        self.pc = {"game": game, "was_above": self.windows.get(game, {}).get("above", False)}
        self.pointer_to(r[0] + x, r[1] + y)
        self.kwin.load("desktoptv_pcmode", PCMODE_JS, {
            "game": game, "keep": self.keepfront, "x": r[0], "y": r[1], "w": r[2], "h": r[3],
            "px": r[0] + x, "py": r[1] + y, "outs": sorted(self._shown_outputs())})
        self.keys.grab(self.qt_keys)
        log("PC mode on")

    def _pc_off(self, focus_game, release, target=None):
        pc, self.pc = self.pc, None
        if pc is None:
            return
        self.kwin.unload("desktoptv_pcmode")
        self.keys.release()
        game = self.windows.get(pc["game"])
        if game:
            if target is not None:
                c = game["client"]
                to = (c[0] + target[0] * c[2], c[1] + target[1] * c[3])
            else:  # released: middle of the game's monitor
                g = self.outputs.get(game["output"])
                to = (g[0] + g[2] / 2, g[1] + g[3] / 2) if g else None
            if to:
                self.pointer_to(*to)
        self.kwin.once(PCOFF_JS, {"game": pc["game"], "keep": self.keepfront, "was_above": pc["was_above"], "focus": focus_game})
        log("PC mode off")

    def _check_focus(self):
        # Like the Windows version: PC mode ends when something on another monitor gets the focus
        if not self.pc or not self.active or self.active == self.pc["game"]:
            return
        w = self.windows.get(self.active)
        shown = self._shown_outputs()
        if w and shown and w["output"] and w["output"] not in shown and not w["desktop"] and w["class"] != "plasmashell":
            self._send("pc_lost the game left the screen - cursor released")

    def _global_key(self, action):
        if self.pc and action in ("pc", "hide"):
            self._send("key " + action)


def main():
    ap = argparse.ArgumentParser(description="DesktopTV helper: picture, PC mode and sound for the add-on in Proton.")
    ap.add_argument("--port", type=int, default=47474)
    ap.add_argument("--size", default="", help="picture size sent to the game, e.g. 1920x1080 (default: the monitor's own size)")
    ap.add_argument("--fps", type=int, default=30, help="until the add-on sends its FPS= setting")
    ap.add_argument("--choose", action="store_true", help="forget the remembered monitor/window and pointer permission, ask again")
    ap.add_argument("--no-control", action="store_true", help="do not ask for pointer control (no PC mode)")
    args = ap.parse_args()
    if args.choose:
        for f in (TOKEN_FILE, TOKEN_FILE_SC):
            if os.path.exists(f):
                os.remove(f)

    DBusGMainLoop(set_as_default=True)
    loop = GLib.MainLoop()
    bus = dbus.SessionBus()
    helper = Helper(bus, args, loop)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        GLibUnix.signal_add(GLib.PRIORITY_HIGH, sig, helper._quit)
    helper.start()
    loop.run()
    helper.shutdown()
    log("stopped (sound given back)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
