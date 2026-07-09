#!/usr/bin/env bash
# One-shot build for the VietKi Linux IBus engine on Ubuntu/Debian (Phase 4.1).
set -eu

cd "$(dirname "$0")"

# --- dependencies ----------------------------------------------------------
# Build tools + IBus/GLib development headers.
PKGS="build-essential cmake pkg-config
dpkg-dev
libibus-1.0-dev libglib2.0-dev"

missing=0
for p in $PKGS; do
  dpkg -s "$p" >/dev/null 2>&1 || missing=1
done

if [ "$missing" -ne 0 ]; then
  echo "Installing build dependencies (needs sudo)..."
  sudo apt-get update
  sudo apt-get install -y $PKGS
fi

# --- configure + build -----------------------------------------------------
# On WSL Windows mounts (/mnt/c, /mnt/d, ...) CMake's configure_file() can fail
# to chmod, so keep the build tree on WSL's native filesystem and copy the
# binary back beside the source.
SRC_DIR="$(pwd)"
BUILD_DIR="${VIETKI_BUILD_DIR:-${HOME}/.cache/vietki/build-linux}"
OUT_DIR="${SRC_DIR}/build-linux"
PKG_ROOT="${BUILD_DIR}/pkgroot"
PACKAGE_NAME="ibus-vietki"
DEFAULT_VERSION="0.6.0"
if [ -n "${VIETKI_VERSION:-}" ]; then
  VERSION="$VIETKI_VERSION"
else
  VERSION="$(git -C "$SRC_DIR" describe --tags --always 2>/dev/null || true)"
  VERSION="$(printf '%s' "$VERSION" | sed 's/^[vV]//; s/[^A-Za-z0-9.+:~-]/-/g')"
fi
VERSION="${VERSION:-$DEFAULT_VERSION}"
if ! printf '%s' "$VERSION" | grep -Eq '^[0-9]'; then
  VERSION="${DEFAULT_VERSION}+git.${VERSION}"
fi
ARCH="$(dpkg --print-architecture)"

if [ -d "$BUILD_DIR" ]; then
  echo "Removing existing build directory: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$OUT_DIR"

echo "Configuring Linux Release build..."
cmake -B "$BUILD_DIR" -S "$SRC_DIR" -DVIETKI_LINUX=ON -DCMAKE_BUILD_TYPE=Release

echo "Building ibus-engine-vietki..."
cmake --build "$BUILD_DIR" --parallel

cp "$BUILD_DIR/ibus-engine-vietki" "$OUT_DIR/ibus-engine-vietki"
chmod +x "$OUT_DIR/ibus-engine-vietki" 2>/dev/null || true

echo "Packaging $PACKAGE_NAME .deb..."
rm -rf "$PKG_ROOT"
install -Dm755 "$BUILD_DIR/ibus-engine-vietki" \
  "$PKG_ROOT/usr/lib/ibus-vietki/ibus-engine-vietki"
install -Dm644 "$SRC_DIR/assets/VietKi.png" \
  "$PKG_ROOT/usr/share/ibus-vietki/icon.png"
install -Dm644 "$SRC_DIR/packaging/vietki.xml" \
  "$PKG_ROOT/usr/share/ibus/component/vietki.xml"

install -d "$PKG_ROOT/DEBIAN"
cat > "$PKG_ROOT/DEBIAN/control" <<EOF
Package: $PACKAGE_NAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: VietKi <vietki@example.invalid>
Depends: libc6, libstdc++6, libibus-1.0-5, libglib2.0-0
Recommends: ibus
Description: Vietnamese input method for IBus (Telex/VNI)
 VietKi is an IBus engine for typing Vietnamese. It renders the syllable being
 composed as a preedit and commits it on a word break, so it works on both X11
 and Wayland. Enable it in Settings -> Keyboard -> Input Sources after install.
EOF

cat > "$PKG_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
# Pick up the new component and (re)start the daemon so VietKi appears in the
# input-source list. Best-effort: ibus may not be running during install.
if command -v ibus >/dev/null 2>&1; then
  ibus restart >/dev/null 2>&1 || true
fi
exit 0
EOF
chmod 755 "$PKG_ROOT/DEBIAN/postinst"

cat > "$PKG_ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v ibus >/dev/null 2>&1; then
  ibus restart >/dev/null 2>&1 || true
fi
exit 0
EOF
chmod 755 "$PKG_ROOT/DEBIAN/postrm"

DEB_PATH="$OUT_DIR/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$PKG_ROOT" "$DEB_PATH"

echo
echo "Done: build-linux/ibus-engine-vietki"
echo "Debian package: $DEB_PATH"
echo "Install it with: sudo apt install ./$DEB_PATH"
echo
echo "After install: run 'ibus restart' (or log out/in), then add 'VietKi'"
echo "in Settings -> Keyboard -> Input Sources. Switch Viet/Anh with Super+Space."
