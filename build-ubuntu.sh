#!/usr/bin/env bash
# One-shot build for the VietKi Linux/X11 desktop shell on Ubuntu/Debian.
set -euo pipefail

cd "$(dirname "$0")"

# --- dependencies ----------------------------------------------------------
# Build tools + X11 (libx11, libXtst = XTest & XRecord) + GTK3 + AppIndicator.
PKGS=(
  build-essential cmake pkg-config
  dpkg-dev
  libx11-dev libxtst-dev
  libgtk-3-dev libayatana-appindicator3-dev
)

missing=0
for p in "${PKGS[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || missing=1
done

if [ "$missing" -ne 0 ]; then
  echo "Installing build dependencies (needs sudo)..."
  sudo apt-get update
  sudo apt-get install -y "${PKGS[@]}"
fi

# --- configure + build -----------------------------------------------------
# CMake 4.2 configure_file() preserves file permissions with chmod(2). On WSL
# Windows mounts (/mnt/c, /mnt/d, ...), chmod may fail with "Operation not
# permitted", so keep the actual build tree on WSL's native filesystem and copy
# the final binary back beside the source.
SRC_DIR="$(pwd)"
BUILD_DIR="${VIETKI_BUILD_DIR:-${HOME}/.cache/vietki/build-linux}"
OUT_DIR="${SRC_DIR}/build-linux"
PKG_ROOT="${BUILD_DIR}/pkgroot"
PACKAGE_NAME="vietki"
if [ -n "${VIETKI_VERSION:-}" ]; then
  VERSION="$VIETKI_VERSION"
else
  VERSION="$(git -C "$SRC_DIR" describe --tags --always 2>/dev/null || true)"
  VERSION="$(printf '%s' "$VERSION" | sed 's/^[vV]//; s/[^A-Za-z0-9.+:~_-]/-/g')"
fi
VERSION="${VERSION:-0.5.0}"
ARCH="$(dpkg --print-architecture)"

if [ -d "$BUILD_DIR" ]; then
  echo "Removing existing WSL build directory: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$OUT_DIR"

echo "Configuring Linux Release build..."
cmake -B "$BUILD_DIR" -S "$SRC_DIR" -DVIETKI_LINUX=ON -DCMAKE_BUILD_TYPE=Release

echo "Building VietKi..."
cmake --build "$BUILD_DIR" --parallel

cp "$BUILD_DIR/VietKi" "$OUT_DIR/VietKi"
chmod +x "$OUT_DIR/VietKi" 2>/dev/null || true

echo "Packaging VietKi .deb..."
rm -rf "$PKG_ROOT"
install -Dm755 "$BUILD_DIR/VietKi" "$PKG_ROOT/usr/bin/vietki"
install -Dm644 "$SRC_DIR/assets/VietKi.png" \
  "$PKG_ROOT/usr/share/icons/hicolor/512x512/apps/vietki.png"

install -d "$PKG_ROOT/usr/share/applications"
cat > "$PKG_ROOT/usr/share/applications/vietki.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=VietKi
Comment=Vietnamese input method
Exec=vietki
Icon=vietki
Terminal=false
Categories=Utility;GTK;
StartupNotify=false
EOF

install -d "$PKG_ROOT/DEBIAN"
cat > "$PKG_ROOT/DEBIAN/control" <<EOF
Package: $PACKAGE_NAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: VietKi <vietki@example.invalid>
Depends: libc6, libstdc++6, libx11-6, libxtst6, libgtk-3-0, libayatana-appindicator3-1
Description: Vietnamese input helper for Linux/X11
 VietKi runs in the desktop session tray and rewrites Vietnamese keystrokes
 through X11/XRecord and XTest.
EOF

cat > "$PKG_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
exit 0
EOF
chmod 755 "$PKG_ROOT/DEBIAN/postinst"

cat > "$PKG_ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
exit 0
EOF
chmod 755 "$PKG_ROOT/DEBIAN/postrm"

DEB_PATH="$OUT_DIR/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$PKG_ROOT" "$DEB_PATH"

echo
echo "Done: build-linux/VietKi"
echo "Run it with:  ./build-linux/VietKi"
echo "Debian package: $DEB_PATH"
echo "Install it with: sudo apt install ./$(basename "$DEB_PATH")"
