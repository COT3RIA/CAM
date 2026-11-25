#!/bin/bash
# Builds the macOS installer (.pkg) for CAM by COTERIA.
# Run from the repository root after the CMake build:  bash installer/macos/build-pkg.sh
set -euo pipefail

VERSION="1.0.0"
ART="build/WebcamVST_artefacts/Release"
HERE="installer/macos"
WORK="$(mktemp -d)"
OUT="installer_out"

mkdir -p "$OUT"

# --- VST3 payload ---
mkdir -p "$WORK/vst3root/Library/Audio/Plug-Ins/VST3"
cp -R "$ART/VST3/CAM.vst3" "$WORK/vst3root/Library/Audio/Plug-Ins/VST3/"

pkgbuild \
  --root "$WORK/vst3root" \
  --identifier "com.coteria.cam.vst3" \
  --version "$VERSION" \
  --install-location "/" \
  "$WORK/vst3.pkg"

# --- Standalone payload ---
mkdir -p "$WORK/approot/Applications"
cp -R "$ART/Standalone/CAM.app" "$WORK/approot/Applications/"

pkgbuild \
  --root "$WORK/approot" \
  --identifier "com.coteria.cam.standalone" \
  --version "$VERSION" \
  --install-location "/" \
  "$WORK/standalone.pkg"

# --- Final installer (resources/ содержит background.png с логотипом) ---
productbuild \
  --distribution "$HERE/distribution.xml" \
  --resources "$HERE/resources" \
  --package-path "$WORK" \
  "$OUT/CAM-Setup-macOS.pkg"

echo "Created $OUT/CAM-Setup-macOS.pkg"
