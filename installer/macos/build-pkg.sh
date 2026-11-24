#!/bin/bash
# Builds the macOS installer (.pkg) for Webcam Viewer.
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
cp -R "$ART/VST3/Webcam Viewer.vst3" "$WORK/vst3root/Library/Audio/Plug-Ins/VST3/"

pkgbuild \
  --root "$WORK/vst3root" \
  --identifier "com.webcamviewer.vst3" \
  --version "$VERSION" \
  --install-location "/" \
  "$WORK/vst3.pkg"

# --- Standalone payload ---
mkdir -p "$WORK/approot/Applications"
cp -R "$ART/Standalone/Webcam Viewer.app" "$WORK/approot/Applications/"

pkgbuild \
  --root "$WORK/approot" \
  --identifier "com.webcamviewer.standalone" \
  --version "$VERSION" \
  --install-location "/" \
  "$WORK/standalone.pkg"

# --- Final installer (resources/ содержит background.png с логотипом) ---
productbuild \
  --distribution "$HERE/distribution.xml" \
  --resources "$HERE/resources" \
  --package-path "$WORK" \
  "$OUT/WebcamViewer-Setup-macOS.pkg"

echo "Created $OUT/WebcamViewer-Setup-macOS.pkg"
