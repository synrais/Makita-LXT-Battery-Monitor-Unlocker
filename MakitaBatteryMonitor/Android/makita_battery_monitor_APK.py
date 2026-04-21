"""
Makita Battery Monitor v1.0  —  Android
Battery diagnostic monitor for Makita LXT batteries.
Install APK, connect battery monitor via USB OTG cable.
"""

import threading
import time

from kivy.app import App
from kivy.lang import Builder
from kivy.uix.boxlayout import BoxLayout
from kivy.clock import Clock
from kivy.core.window import Window
from kivy.metrics import dp, sp

from usb4a import usb as android_usb
from usbserial4a import serial4a

VERSION   = "1.0.0"
BAUDRATE  = 115200
MAX_LINES = 500

# ─────────────────────────────────────────────
#  Colour mapping  (Kivy markup tags)
# ─────────────────────────────────────────────
def _c(col: str, text: str, bold: bool = False) -> str:
    inner = f"[b]{text}[/b]" if bold else text
    return f"[color={col}]{inner}[/color]"

def colourise(line: str) -> str:
    s = line.strip()
    if not s:
        return ""
    if "UNLOCKED"         in line: return _c("#00e676", line, bold=True)
    if "LOCKED"           in line: return _c("#ff5252", line, bold=True)
    if "ERROR"            in line: return _c("#ffeb3b", line)
    if "WARN"             in line: return _c("#ffeb3b", line)
    if s.startswith("=") or s.startswith("-"):
                                   return _c("#40c4ff", line)
    if "Health"           in line: return _c("#69f0ae", line)
    if "Complete."        in line: return _c("#40c4ff", line, bold=True)
    if "Battery found"    in line: return _c("#69f0ae", line)
    if "Battery detected" in line: return _c("#69f0ae", line)
    if "Battery removed"  in line: return _c("#ff5252", line)
    return _c("#e0e0e0", line)

# ─────────────────────────────────────────────
#  Serial manager  (background thread)
#  UI callbacks dispatched via Clock.schedule_once
# ─────────────────────────────────────────────
class SerialManager:
    def __init__(self, on_line, on_status):
        self._on_line   = on_line
        self._on_status = on_status
        self._port      = None
        self._stop      = threading.Event()

    def start(self):
        self._stop.clear()
        threading.Thread(target=self._run, daemon=True).start()

    def stop(self):
        self._stop.set()
        self._close()

    def send(self, data: bytes):
        try:
            if self._port:
                self._port.write(data)
        except Exception:
            pass

    # ── private helpers ─────────────────────
    def _close(self):
        try:
            if self._port:
                self._port.close()
        except Exception:
            pass
        self._port = None

    def _status(self, msg: str, col: str):
        Clock.schedule_once(lambda dt: self._on_status(msg, col))

    def _emit(self, line: str):
        Clock.schedule_once(lambda dt, l=line: self._on_line(l))

    def _device_present(self) -> bool:
        """Check whether any USB device is still attached."""
        try:
            return len(android_usb.get_usb_device_list()) > 0
        except Exception:
            return False

    def _find_device(self):
        """Block until a USB device appears, updating status each second."""
        attempt = 0
        while not self._stop.is_set():
            devs = android_usb.get_usb_device_list()
            if devs:
                return devs[0]
            attempt += 1
            self._status(
                f"Waiting for USB device...  ({attempt}s)", "#ffeb3b"
            )
            time.sleep(1.0)
        return None

    # ── main connection / read loop ──────────
    def _run(self):
        reconnect_count = 0

        while not self._stop.is_set():
            try:
                # ── wait for device ──────────────────
                dev = self._find_device()
                if dev is None:
                    return  # stop() was called

                # ── request USB permission ───────────
                if not android_usb.has_usb_permission(dev):
                    self._status("Requesting USB permission...", "#ffeb3b")
                    android_usb.request_usb_permission(dev)
                    time.sleep(2.0)
                    continue

                # ── open port ────────────────────────
                name = dev.getDeviceName()
                self._port = serial4a.get_serial_port(
                    name, BAUDRATE, 8, "N", 1, timeout=1
                )

                label = name.split("/")[-1]
                if reconnect_count == 0:
                    self._status(f"●  Connected  {label}", "#00e676")
                else:
                    self._status(
                        f"●  Reconnected  {label}  (#{reconnect_count})", "#00e676"
                    )

                # Wait for the Arduino to finish its reset/boot sequence.
                # USB enumeration toggles DTR which resets the MCU — without
                # this delay the 's' arrives before the firmware is ready and
                # triggers a redundant second scan.
                time.sleep(2.0)

                try:
                    self._port.reset_input_buffer()
                except AttributeError:
                    pass

                self._port.write(b"s")  # auto-scan on connect

                # ── read loop ────────────────────────
                empty_streak = 0
                while not self._stop.is_set():
                    raw = self._port.readline()

                    if raw:
                        empty_streak = 0
                        line = raw.decode("utf-8", errors="replace").rstrip()
                        if line:
                            self._emit(line)
                    else:
                        # readline() returned empty — check device still present
                        empty_streak += 1
                        if empty_streak >= 5 and not self._device_present():
                            raise IOError("USB device no longer present")

            except Exception:
                pass

            finally:
                self._close()

            if self._stop.is_set():
                return

            # ── disconnected — show status and retry ─
            reconnect_count += 1
            self._status(
                f"○  Disconnected — reconnecting...  (#{reconnect_count})", "#ff5252"
            )
            time.sleep(2.0)

# ─────────────────────────────────────────────
#  KV layout
# ─────────────────────────────────────────────
KV = """
<RootLayout>:
    orientation: 'vertical'
    spacing: 0

    canvas.before:
        Color:
            rgba: 0.071, 0.071, 0.071, 1
        Rectangle:
            pos: self.pos
            size: self.size

    # Header ─────────────────────────────────
    BoxLayout:
        orientation: 'horizontal'
        size_hint_y: None
        height: dp(52)
        padding: [dp(14), dp(8), dp(8), dp(8)]
        spacing: dp(6)

        canvas.before:
            Color:
                rgba: 0.118, 0.118, 0.118, 1
            Rectangle:
                pos: self.pos
                size: self.size

        Label:
            markup: True
            text: "[b]Makita Battery Monitor[/b]"
            font_size: sp(13)
            color: 0.88, 0.88, 0.88, 1
            halign: 'left'
            valign: 'middle'
            size_hint_x: 0.40
            text_size: self.size

        Label:
            id: status_lbl
            markup: True
            text: "[color=#ffeb3b]Waiting for USB...[/color]"
            font_size: sp(11)
            halign: 'left'
            valign: 'middle'
            size_hint_x: 1
            text_size: self.size

        Button:
            text: 'Rescan'
            size_hint: None, None
            size: dp(68), dp(36)
            font_size: sp(12)
            color: 0.88, 0.88, 0.88, 1
            background_color: 0.22, 0.22, 0.22, 1
            background_normal: ''
            on_press: app.rescan()

        Button:
            text: 'Clear'
            size_hint: None, None
            size: dp(56), dp(36)
            font_size: sp(12)
            color: 0.88, 0.88, 0.88, 1
            background_color: 0.18, 0.18, 0.18, 1
            background_normal: ''
            on_press: app.clear_output()

        Button:
            text: 'Quit'
            size_hint: None, None
            size: dp(50), dp(36)
            font_size: sp(12)
            color: 1, 0.4, 0.4, 1
            background_color: 0.22, 0.10, 0.10, 1
            background_normal: ''
            on_press: app.quit_app()

    # Separator ──────────────────────────────
    Widget:
        size_hint_y: None
        height: dp(1)
        canvas:
            Color:
                rgba: 0.22, 0.22, 0.22, 1
            Rectangle:
                pos: self.pos
                size: self.size

    # Output area ────────────────────────────
    ScrollView:
        id: scroll
        do_scroll_x: False
        bar_width: dp(3)
        bar_color: 0.35, 0.35, 0.35, 1
        bar_inactive_color: 0.15, 0.15, 0.15, 1

        Label:
            id: output_lbl
            markup: True
            font_size: sp(12)
            color: 0.88, 0.88, 0.88, 1
            valign: 'top'
            halign: 'left'
            size_hint_y: None
            padding: [dp(12), dp(10)]
            text_size: self.width - dp(8), None
            size: self.texture_size
"""

Builder.load_string(KV)

# ─────────────────────────────────────────────
#  Root widget
# ─────────────────────────────────────────────
PLACEHOLDER = "[color=#333333]Connect the battery monitor via USB OTG cable...[/color]"

class RootLayout(BoxLayout):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._lines = []
        Clock.schedule_once(self._init_placeholder, 0.1)

    def _init_placeholder(self, dt):
        self.ids.output_lbl.text = PLACEHOLDER

    def append_line(self, line: str):
        coloured = colourise(line)
        if not coloured:
            return
        ts = time.strftime("%H:%M:%S")
        entry = f"[color=#444444]{ts}[/color]  {coloured}"
        self._lines.append(entry)
        if len(self._lines) > MAX_LINES:
            self._lines = self._lines[-MAX_LINES:]
        self.ids.output_lbl.text = "\n".join(self._lines)
        Clock.schedule_once(
            lambda dt: setattr(self.ids.scroll, "scroll_y", 0), 0.05
        )

    def clear_output(self):
        self._lines = []
        self.ids.output_lbl.text = PLACEHOLDER

    def set_status(self, msg: str, colour: str):
        self.ids.status_lbl.text = f"[color={colour}]{msg}[/color]"

# ─────────────────────────────────────────────
#  App entry point
# ─────────────────────────────────────────────
class MakitaBatteryMonitorApp(App):
    def build(self):
        Window.clearcolor = (0.071, 0.071, 0.071, 1)
        self._root   = RootLayout()
        self._serial = SerialManager(
            on_line=self._root.append_line,
            on_status=self._root.set_status,
        )
        self._serial.start()
        return self._root

    def rescan(self):
        self._serial.send(b"s")

    def clear_output(self):
        self._root.clear_output()

    def quit_app(self):
        self._serial.stop()
        self.stop()

    def on_stop(self):
        self._serial.stop()


if __name__ == "__main__":
    MakitaBatteryMonitorApp().run()
