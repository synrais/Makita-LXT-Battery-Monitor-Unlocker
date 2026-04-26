[app]

title           = Makita Battery Monitor
package.name    = makitabatterymonitor
package.domain  = com.makitamonitor

source.dir      = .
source.include_exts = py

version         = 1.0.0

# Kivy + Android USB serial.
# pillow removed — no images in this app.
# pyserial is PC-only and never imported on Android.
requirements    = python3,kivy==2.3.0,usb4a,usbserial4a

orientation     = portrait
fullscreen      = 0

# Android ────────────────────────────────────────────────────
android.api         = 34
android.minapi      = 26
android.ndk         = 25b

android.archs       = arm64-v8a, armeabi-v7a

# Blacklist recipes we don't use to trim the final APK.
# sqlite3   — no database needed
# sdl2_mixer — no audio needed
android.blacklist_requirements = sqlite3,sdl2_mixer

# USB Host — permission is handled at runtime by usb4a.
# android.features is intentionally omitted: p4a does not support
# the --feature flag and USB host is standard on all modern Android.

android.accept_sdk_license = True
android.allow_backup        = False

# Log level: 2 = info, 3 = debug
[buildozer]
log_level = 2
warn_on_root = 1
