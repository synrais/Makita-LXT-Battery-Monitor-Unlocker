#!/bin/bash
# Makita Battery Monitor — APK Builder
# Drop this file in the same folder as makita_battery_monitor_APK.py
# and run via the .bat, or directly from WSL.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$HOME/makita_monitor_build"

echo
echo "============================================"
echo "  Makita Battery Monitor — APK Builder"
echo "============================================"
echo

# ── 1. System dependencies ───────────────────────────────────
echo "[1/4] Installing system dependencies..."

sudo dpkg --remove-architecture armhf 2>/dev/null || true
sudo dpkg --remove-architecture arm64 2>/dev/null || true
sudo rm -f /usr/lib/python3*/EXTERNALLY-MANAGED

sudo apt-get update -qq
sudo apt-get install -y -qq \
    python3-pip python3-venv git zip unzip openjdk-17-jdk \
    autoconf libtool pkg-config zlib1g-dev \
    libncurses5-dev libncursesw5-dev cmake \
    libffi-dev libssl-dev build-essential

export JAVA_HOME=$(dirname $(dirname $(readlink -f $(which java))))
echo "    JAVA_HOME = $JAVA_HOME"

# ── 2. Python tools ──────────────────────────────────────────
echo
echo "[2/4] Installing Buildozer and Cython..."
pip3 install --break-system-packages --quiet buildozer "cython==0.29.37" setuptools
export PATH="$HOME/.local/bin:$PATH"
export BUILDOZER="$HOME/.local/bin/buildozer"

# ── 3. Prepare build directory ───────────────────────────────
echo
echo "[3/4] Preparing build directory at $BUILD_DIR..."
mkdir -p "$BUILD_DIR"
cp "$SCRIPT_DIR/makita_battery_monitor_APK.py" "$BUILD_DIR/main.py"
cp "$SCRIPT_DIR/buildozer.spec"                "$BUILD_DIR/"

# ── 4. Build ─────────────────────────────────────────────────
echo
echo "[4/4] Running Buildozer..."
echo "      First run downloads the Android SDK + NDK (~1 GB)."
echo "      This takes 20-40 minutes. Subsequent builds are fast."
echo
cd "$BUILD_DIR"
"$BUILDOZER" android debug

# ── Copy APK back to Windows project folder ──────────────────
APK=$(ls "$BUILD_DIR"/bin/*.apk 2>/dev/null | head -1)
if [ -n "$APK" ]; then
    mkdir -p "$SCRIPT_DIR/dist"
    cp "$APK" "$SCRIPT_DIR/dist/"
    echo
    echo "============================================"
    echo "  Done!"
    echo "  APK saved to:"
    echo "  $(wslpath -w "$SCRIPT_DIR/dist/$(basename "$APK")")"
    echo
    echo "  Install on Android:"
    echo "  adb install \"$SCRIPT_DIR/dist/$(basename "$APK")\""
    echo "  or copy the APK to your phone and open it."
    echo "============================================"
else
    echo
    echo "ERROR: No APK found in $BUILD_DIR/bin/"
    echo "Check the buildozer output above for errors."
    exit 1
fi