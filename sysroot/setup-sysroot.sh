#!/bin/bash
# Downloads minimal ARM sysroot (libc6, libstdc++6, libgcc-s1) from Debian bookworm
# and overlays Eloquence libraries with necessary ELF patches.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRIDGE_DIR="$(dirname "$SCRIPT_DIR")"
PROJECT_DIR="$(dirname "$BRIDGE_DIR")"
SYSROOT="$SCRIPT_DIR/armhf"
ROOTFS="$PROJECT_DIR/rootfs"

# Use bookworm (glibc 2.36). Needed because:
# - The cross-compiler's CRT startup requires GLIBC_2.34 (__libc_start_main)
# - bullseye (glibc 2.31) is too old for the bridge binary
# - libeci.so's old version requirements (GLIBC_2.0/2.1) are patched to GLIBC_2.4
#   so bookworm's merged libdl/libpthread stubs (which export GLIBC_2.4) work fine
MIRROR="http://deb.debian.org/debian"
DIST="bookworm"
ARCH="armhf"
DEBS_DIR="$SCRIPT_DIR/.debs"

mkdir -p "$DEBS_DIR" "$SYSROOT"

# Packages to download
PACKAGES=(
    "libc6"
    "libstdc++6"
    "libgcc-s1"
)

echo "=== Downloading armhf packages from Debian $DIST ==="

# Download and cache the Packages index
PACKAGES_FILE="$DEBS_DIR/Packages-$DIST"
if [ ! -f "$PACKAGES_FILE" ]; then
    echo "  Downloading package index..."
    curl -sL "$MIRROR/dists/$DIST/main/binary-$ARCH/Packages.xz" -o "$DEBS_DIR/Packages-$DIST.xz"
    xz -d -k "$DEBS_DIR/Packages-$DIST.xz"
fi

for pkg in "${PACKAGES[@]}"; do
    echo "  Fetching $pkg..."
    PKG_URL=$(awk -v pkg="$pkg" '
        /^Package: / { name=$2 }
        /^Filename: / { if (name == pkg) { print $2; exit } }
    ' "$PACKAGES_FILE")
    if [ -z "$PKG_URL" ]; then
        echo "    ERROR: could not find $pkg in $DIST/$ARCH" >&2
        exit 1
    fi
    DEB_FILE="$DEBS_DIR/$(basename "$PKG_URL")"
    if [ ! -f "$DEB_FILE" ]; then
        curl -sL -o "$DEB_FILE" "$MIRROR/$PKG_URL"
    fi
    echo "    Extracting $DEB_FILE..."
    dpkg-deb -x "$DEB_FILE" "$SYSROOT/"
done

echo ""
echo "=== Overlaying Eloquence libraries ==="

# Copy ECI libraries
cp -v "$ROOTFS/usr/lib/libeci.so" "$SYSROOT/usr/lib/"
cp -v "$ROOTFS/usr/lib/enu.so" "$SYSROOT/usr/lib/"

# Patch ELF OS/ABI: old ARM toolchain sets 0x61 (ARM), modern glibc rejects it.
# Change byte 7 to 0x00 (SYSV) so ld.so will load them.
echo "  Patching ELF OS/ABI in libeci.so and enu.so..."
printf '\x00' | dd of="$SYSROOT/usr/lib/libeci.so" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
printf '\x00' | dd of="$SYSROOT/usr/lib/enu.so" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Patch GLIBC version requirements: ARM glibc starts at GLIBC_2.4, but libeci.so
# references GLIBC_2.0/2.1/2.1.3/2.3.2 (from the original x86-like toolchain).
# Rewrite both the version strings and their ELF hashes to GLIBC_2.4.
echo "  Patching GLIBC version strings and hashes..."
python3 "$SCRIPT_DIR/patch-glibc-versions.py" \
    "$SYSROOT/usr/lib/libeci.so" \
    "$SYSROOT/usr/lib/enu.so"

# Build SJLJ compat shim: old ARM libeci.so uses setjmp-based C++ exception
# handling, but modern ARM libstdc++/libgcc only provide DWARF-based unwinding.
echo "  Building SJLJ compatibility library..."
arm-linux-gnueabihf-gcc --sysroot="$SYSROOT" -shared -fPIC \
    -o "$SYSROOT/usr/lib/libsjlj_compat.so" \
    "$BRIDGE_DIR/arm/sjlj_compat.c" \
    -Wl,--version-script="$BRIDGE_DIR/arm/sjlj_compat.map" \
    -Wl,-soname,libsjlj_compat.so

# Copy ECI config
mkdir -p "$SYSROOT/etc"
cp -v "$ROOTFS/etc/eci.ini" "$SYSROOT/etc/"

echo ""
echo "=== Verifying sysroot ==="

# Find the dynamic linker
LDSO=$(find "$SYSROOT" -name "ld-linux-armhf.so*" | head -1)
if [ -z "$LDSO" ]; then
    echo "ERROR: ld-linux-armhf.so not found in sysroot" >&2
    exit 1
fi
echo "Dynamic linker: $LDSO"

# Verify libeci.so dependencies
echo "Checking libeci.so dependencies..."
if command -v qemu-arm &>/dev/null; then
    qemu-arm -L "$SYSROOT" "$LDSO" --list "$SYSROOT/usr/lib/libeci.so" || {
        echo "WARNING: ldd check failed (may still work)" >&2
    }
else
    echo "qemu-arm not found, skipping runtime verification"
fi

echo ""
echo "=== Sysroot ready at $SYSROOT ==="
du -sh "$SYSROOT"
echo ""
echo "Contents:"
find "$SYSROOT" -name "*.so*" -o -name "*.ini" | sort
