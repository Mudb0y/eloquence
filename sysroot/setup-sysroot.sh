#!/bin/bash
# Downloads the LevelStar Icon PDA image to extract Eloquence libraries,
# downloads a minimal ARM sysroot (libc6, libstdc++6, libgcc-s1) from Debian
# bookworm, and applies necessary ELF patches.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BRIDGE_DIR="$(dirname "$SCRIPT_DIR")"
SYSROOT="$SCRIPT_DIR/armhf"
CACHE_DIR="$SCRIPT_DIR/.debs"

# LevelStar Icon PDA firmware image (contains Eloquence V6.1 ARM libraries)
LSI_URL="http://tech.aph.org/2.2.53.lsi"
LSI_FILE="$CACHE_DIR/2.2.53.lsi"

# Use bookworm (glibc 2.36). Needed because:
# - The cross-compiler's CRT startup requires GLIBC_2.34 (__libc_start_main)
# - bullseye (glibc 2.31) is too old for the bridge binary
# - libeci.so's old version requirements (GLIBC_2.0/2.1) are patched to GLIBC_2.4
#   so bookworm's merged libdl/libpthread stubs (which export GLIBC_2.4) work fine
MIRROR="http://deb.debian.org/debian"
DIST="bookworm"
ARCH="armhf"

mkdir -p "$CACHE_DIR" "$SYSROOT"

# --- Step 1: Extract Eloquence libraries from the PDA image ---

echo "=== Extracting Eloquence libraries from PDA image ==="

ROOTFS_DIR="$CACHE_DIR/rootfs"

if [ -f "$ROOTFS_DIR/usr/lib/libeci.so" ] && \
   [ -f "$ROOTFS_DIR/usr/lib/enu.so" ] && \
   [ -f "$ROOTFS_DIR/etc/eci.ini" ]; then
    echo "  Using cached extraction in $ROOTFS_DIR"
else
    # Download firmware image
    if [ ! -f "$LSI_FILE" ]; then
        echo "  Downloading PDA image (~56MB)..."
        curl -L -o "$LSI_FILE" "$LSI_URL"
    else
        echo "  Using cached $LSI_FILE"
    fi

    # Extract JFFS2 root filesystem from the zip
    echo "  Extracting root.bin from zip..."
    unzip -o -d "$CACHE_DIR" "$LSI_FILE" root.bin

    # Extract files from JFFS2 image using jefferson
    if ! command -v jefferson &>/dev/null; then
        echo "ERROR: jefferson not found. Install with: pip install jefferson" >&2
        exit 1
    fi
    echo "  Extracting JFFS2 filesystem (this takes a moment)..."
    jefferson -f -d "$ROOTFS_DIR" "$CACHE_DIR/root.bin"

    # Clean up the large intermediate file
    rm -f "$CACHE_DIR/root.bin"

    # Verify extraction
    for f in usr/lib/libeci.so usr/lib/enu.so etc/eci.ini; do
        if [ ! -f "$ROOTFS_DIR/$f" ]; then
            echo "ERROR: $f not found in extracted rootfs" >&2
            exit 1
        fi
    done
    echo "  Eloquence files extracted successfully"
fi

# --- Step 2: Download armhf packages from Debian ---

PACKAGES=(
    "libc6"
    "libstdc++6"
    "libgcc-s1"
)

echo ""
echo "=== Downloading armhf packages from Debian $DIST ==="

PACKAGES_FILE="$CACHE_DIR/Packages-$DIST"
if [ ! -f "$PACKAGES_FILE" ]; then
    echo "  Downloading package index..."
    curl -sL "$MIRROR/dists/$DIST/main/binary-$ARCH/Packages.xz" -o "$CACHE_DIR/Packages-$DIST.xz"
    xz -d -k "$CACHE_DIR/Packages-$DIST.xz"
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
    DEB_FILE="$CACHE_DIR/$(basename "$PKG_URL")"
    if [ ! -f "$DEB_FILE" ]; then
        curl -sL -o "$DEB_FILE" "$MIRROR/$PKG_URL"
    fi
    echo "    Extracting $DEB_FILE..."
    dpkg-deb -x "$DEB_FILE" "$SYSROOT/"
done

# --- Step 3: Overlay Eloquence libraries and apply patches ---

echo ""
echo "=== Overlaying Eloquence libraries ==="

cp -v "$ROOTFS_DIR/usr/lib/libeci.so" "$SYSROOT/usr/lib/"
cp -v "$ROOTFS_DIR/usr/lib/enu.so" "$SYSROOT/usr/lib/"
mkdir -p "$SYSROOT/etc"
cp -v "$ROOTFS_DIR/etc/eci.ini" "$SYSROOT/etc/"

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

# --- Step 4: Verify ---

echo ""
echo "=== Verifying sysroot ==="

LDSO=$(find "$SYSROOT" -name "ld-linux-armhf.so*" | head -1)
if [ -z "$LDSO" ]; then
    echo "ERROR: ld-linux-armhf.so not found in sysroot" >&2
    exit 1
fi
echo "Dynamic linker: $LDSO"

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
