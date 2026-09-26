#!/bin/bash

abort()
{
    cd -
    echo "-----------------------------------------------"
    echo "Kernel compilation failed! Exiting..."
    echo "-----------------------------------------------"
    exit -1
}

unset_flags()
{
    cat << EOF
Usage: $(basename "$0") [options]
Options:
    -m, --model [value]     Specify the model code of the phone
    -k, --ksu [Y/n]         Include KernelSU
    -r, --recovery [y/N]    Compile kernel for an Android Recovery
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model|-m)
            MODEL="$2"
            shift 2
            ;;
        --ksu|-k)
            KSU_OPTION="$2"
            shift 2
            ;;
        --recovery|-r)
            RECOVERY_OPTION="$2"
            shift 2
            ;;
        *)
            unset_flags
            exit 1
            ;;
    esac
done

echo "Preparing the build environment..."

pushd "$(dirname "$0")" > /dev/null || exit 1

CORES=$(grep -c processor /proc/cpuinfo)

# ============================================================
# LLVM TOOLCHAIN
# ============================================================

CLANG_DIR="$PWD/toolchain/clang-r547379"
CLANG_BIN="$CLANG_DIR/bin"

echo "-----------------------------------------------"
echo "Checking LLVM toolchain..."
echo "-----------------------------------------------"

# Check whether the complete toolchain is available.
# Do not only check clang: llvm-ar and llvm-nm are also
# required by the kernel build system.
TOOLCHAIN_MISSING=false

for tool in \
    clang-20 \
    llvm-ar \
    llvm-nm \
    llvm-objcopy \
    llvm-objdump \
    llvm-strip \
    ld.lld
do
    if [[ ! -x "$CLANG_BIN/$tool" ]]; then
        echo "Missing LLVM tool: $CLANG_BIN/$tool"
        TOOLCHAIN_MISSING=true
    fi
done

# Download the toolchain if anything is missing.
if [[ "$TOOLCHAIN_MISSING" == true ]]; then
    echo "-----------------------------------------------"
    echo "LLVM toolchain incomplete or missing!"
    echo "Downloading clang-r547379..."
    echo "-----------------------------------------------"

    rm -rf "$CLANG_DIR"
    mkdir -p "$CLANG_DIR"

    pushd "$CLANG_DIR" > /dev/null || exit 1

    TOOLCHAIN_ARCHIVE="clang-r547379.tar.gz"

    curl -fL \
        --retry 5 \
        --retry-delay 5 \
        --retry-all-errors \
        "https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-r547379.tar.gz" \
        -o "$TOOLCHAIN_ARCHIVE" || {
            echo "-----------------------------------------------"
            echo "Failed to download LLVM toolchain!"
            echo "-----------------------------------------------"
            popd > /dev/null
            exit 1
        }

    echo "Extracting LLVM toolchain..."

    tar xf "$TOOLCHAIN_ARCHIVE" || {
        echo "-----------------------------------------------"
        echo "Failed to extract LLVM toolchain!"
        echo "-----------------------------------------------"
        rm -f "$TOOLCHAIN_ARCHIVE"
        popd > /dev/null
        exit 1
    }

    rm -f "$TOOLCHAIN_ARCHIVE"

    popd > /dev/null || exit 1
fi

# Make sure LLVM binaries are executable.
chmod +x \
    "$CLANG_BIN/clang-20" \
    "$CLANG_BIN/llvm-ar" \
    "$CLANG_BIN/llvm-nm" \
    "$CLANG_BIN/llvm-objcopy" \
    "$CLANG_BIN/llvm-objdump" \
    "$CLANG_BIN/llvm-strip" \
    "$CLANG_BIN/ld.lld" \
    2>/dev/null

# Final toolchain validation.
for tool in \
    clang-20 \
    llvm-ar \
    llvm-nm \
    llvm-objcopy \
    llvm-objdump \
    llvm-strip \
    ld.lld
do
    if [[ ! -x "$CLANG_BIN/$tool" ]]; then
        echo "-----------------------------------------------"
        echo "ERROR: LLVM toolchain is incomplete!"
        echo "Missing:"
        echo "  $CLANG_BIN/$tool"
        echo "-----------------------------------------------"
        exit 1
    fi
done

# Put the bundled LLVM first in PATH.
export PATH="$CLANG_BIN:$PATH"

echo "LLVM toolchain verified:"
echo "  clang:    $CLANG_BIN/clang-20"
echo "  llvm-ar:  $CLANG_BIN/llvm-ar"
echo "  llvm-nm:  $CLANG_BIN/llvm-nm"
echo "  objcopy:  $CLANG_BIN/llvm-objcopy"
echo "  objdump:  $CLANG_BIN/llvm-objdump"
echo "  strip:    $CLANG_BIN/llvm-strip"
echo "  linker:   $CLANG_BIN/ld.lld"
echo "-----------------------------------------------"

# ============================================================
# MODEL CONFIGURATION
# ============================================================

case "$MODEL" in
beyond0lte)
    BOARD=SRPRI28A016KU
    SOC=exynos9820
    ;;
beyond1lte)
    BOARD=SRPRI28B016KU
    SOC=exynos9820
    ;;
beyond2lte)
    BOARD=SRPRI17C016KU
    SOC=exynos9820
    ;;
beyondx)
    BOARD=SRPSC04B014KU
    SOC=exynos9820
    ;;
d1)
    BOARD=SRPSD26B009KU
    SOC=exynos9825
    ;;
d1xks)
    BOARD=SRPSD23A002KU
    SOC=exynos9825
    ;;
d2s)
    BOARD=SRPSC14B009KU
    SOC=exynos9825
    ;;
d2x)
    BOARD=SRPSC14C009KU
    SOC=exynos9825
    ;;
d2xks)
    BOARD=SRPSD23C002KU
    SOC=exynos9825
    ;;
*)
    unset_flags
    exit 1
    ;;
esac

# ============================================================
# RECOVERY / KSU OPTIONS
# ============================================================

if [[ "$RECOVERY_OPTION" == "y" ]]; then
    RECOVERY=recovery.config
    KSU_OPTION=n
fi

if [[ -z "$KSU_OPTION" ]]; then
    read -r -p "Include KernelSU (y/N): " KSU_OPTION
fi

if [[ "$KSU_OPTION" == "y" ]]; then
    KSU=ksu.config
fi

# ============================================================
# OUTPUT DIRECTORIES
# ============================================================

rm -rf "build/out/$MODEL"

mkdir -p \
    "build/out/$MODEL/zip/files" \
    "build/out/$MODEL/zip/META-INF/com/google/android"

# ============================================================
# MAKE ARGUMENTS
# ============================================================

# IMPORTANT:
# Explicitly point Kbuild to our bundled LLVM installation.
#
# This prevents llvm-ar / llvm-nm from depending on whatever
# happens to be installed in the GitHub Actions runner PATH.
#
# The trailing '/' after CLANG_BIN is intentional.
MAKE_ARGS=(
    "LLVM=$CLANG_BIN/"
    "LLVM_IAS=1"
    "ARCH=arm64"
    "O=out"
)

# ============================================================
# BUILD INFORMATION
# ============================================================

echo "-----------------------------------------------"
echo "Defconfig: $KERNEL_DEFCONFIG"

if [[ -z "$KSU" ]]; then
    echo "KSU: No"
else
    echo "KSU: Yes"
fi

if [[ -z "$RECOVERY" ]]; then
    echo "Recovery: N"
else
    echo "Recovery: Y"
fi

echo "LLVM: $CLANG_BIN/"
echo "Jobs: $CORES"
echo "-----------------------------------------------"

# ============================================================
# GENERATE KERNEL CONFIGURATION
# ============================================================

echo "Building kernel using $KERNEL_DEFCONFIG"
echo "Generating configuration file..."
echo "-----------------------------------------------"

make "${MAKE_ARGS[@]}" \
    -j"$CORES" \
    exynos9820_defconfig \
    "$MODEL.config" \
    "$KSU" \
    "$RECOVERY" || abort

# ============================================================
# BUILD KERNEL
# ============================================================

echo "Building kernel..."
echo "-----------------------------------------------"

make "${MAKE_ARGS[@]}" \
    -j"$CORES" || abort

# ============================================================
# BOOT IMAGE CONSTANTS
# ============================================================

KERNEL_PATH="build/out/$MODEL/Image"

KERNEL_OFFSET=0x00008000
RAMDISK_OFFSET=0xF0000000
SECOND_OFFSET=0xF0000000
TAGS_OFFSET=0x00000100

BASE=0x10000000

CMDLINE='loop.max_part=7'

HASHTYPE=sha1
HEADER_VERSION=1

OS_PATCH_LEVEL=2025-08
OS_VERSION=16.0.0

PAGESIZE=2048

RAMDISK="build/out/$MODEL/ramdisk.cpio.gz"
OUTPUT_FILE="build/out/$MODEL/boot.img"

# ============================================================
# COPY KERNEL IMAGE
# ============================================================

echo "-----------------------------------------------"
echo "Copying kernel image..."
echo "-----------------------------------------------"

if [[ ! -f "out/arch/arm64/boot/Image" ]]; then
    echo "Kernel Image not found!"
    abort
fi

cp "out/arch/arm64/boot/Image" "build/out/$MODEL/Image" || abort

# ============================================================
# BUILD DTB
# ============================================================

echo "-----------------------------------------------"

if [[ "$SOC" == "exynos9820" ]]; then
    echo "Building common exynos9820 Device Tree Blob Image..."
    echo "-----------------------------------------------"

    ./toolchain/mkdtimg \
        cfg_create \
        "build/out/$MODEL/dtb.img" \
        build/dtconfigs/exynos9820.cfg \
        -d out/arch/arm64/boot/dts/exynos || abort
fi

if [[ "$SOC" == "exynos9825" ]]; then
    echo "Building common exynos9825 Device Tree Blob Image..."
    echo "-----------------------------------------------"

    ./toolchain/mkdtimg \
        cfg_create \
        "build/out/$MODEL/dtb.img" \
        build/dtconfigs/exynos9825.cfg \
        -d out/arch/arm64/boot/dts/exynos || abort
fi

echo "-----------------------------------------------"

# ============================================================
# BUILD DTBO
# ============================================================

echo "Building Device Tree Blob Output Image for $MODEL..."
echo "-----------------------------------------------"

./toolchain/mkdtimg \
    cfg_create \
    "build/out/$MODEL/dtbo.img" \
    "build/dtconfigs/$MODEL.cfg" \
    -d out/arch/arm64/boot/dts/samsung || abort

echo "-----------------------------------------------"

# ============================================================
# BUILD RECOVERY / BOOT RAMDISK
# ============================================================

if [[ -z "$RECOVERY" ]]; then

    echo "Building RAMDisk..."
    echo "-----------------------------------------------"

    pushd build/ramdisk > /dev/null || abort

    find . ! -name . | \
        LC_ALL=C sort | \
        cpio -o -H newc -R root:root | \
        gzip > "../out/$MODEL/ramdisk.cpio.gz" || abort

    popd > /dev/null || abort

    echo "-----------------------------------------------"

    # ========================================================
    # CREATE BOOT IMAGE
    # ========================================================

    echo "Creating boot image..."
    echo "-----------------------------------------------"

    ./toolchain/mkbootimg \
        --base "$BASE" \
        --board "$BOARD" \
        --cmdline "$CMDLINE" \
        --hashtype "$HASHTYPE" \
        --header_version "$HEADER_VERSION" \
        --kernel "$KERNEL_PATH" \
        --kernel_offset "$KERNEL_OFFSET" \
        --os_patch_level "$OS_PATCH_LEVEL" \
        --os_version "$OS_VERSION" \
        --pagesize "$PAGESIZE" \
        --ramdisk "$RAMDISK" \
        --ramdisk_offset "$RAMDISK_OFFSET" \
        --second_offset "$SECOND_OFFSET" \
        --tags_offset "$TAGS_OFFSET" \
        -o "$OUTPUT_FILE" || abort

    # ========================================================
    # BUILD FLASHABLE ZIP
    # ========================================================

    echo "Building zip..."
    echo "-----------------------------------------------"

    cp \
        "build/out/$MODEL/boot.img" \
        "build/out/$MODEL/zip/files/boot.img" || abort

    cp \
        "build/out/$MODEL/dtb.img" \
        "build/out/$MODEL/zip/files/dtb.img" || abort

    cp \
        "build/out/$MODEL/dtbo.img" \
        "build/out/$MODEL/zip/files/dtbo.img" || abort

    cp \
        build/update-binary \
        "build/out/$MODEL/zip/META-INF/com/google/android/update-binary" || abort

    cp \
        build/updater-script \
        "build/out/$MODEL/zip/META-INF/com/google/android/updater-script" || abort

    # ========================================================
    # KERNEL VERSION
    # ========================================================

    version=$(
        grep -o \
            'CONFIG_LOCALVERSION="[^"]*"' \
            arch/arm64/configs/exynos9820_defconfig |
        cut -d '"' -f 2
    )

    version=${version:1}

    if [[ "$SOC" == "exynos9825" ]]; then
        version="${version}-N10"
    else
        version="${version}-S10"
    fi

    # ========================================================
    # ZIP NAME
    # ========================================================

    pushd "build/out/$MODEL/zip" > /dev/null || abort

    DATE=$(date +"%d-%m-%Y_%H-%M-%S")

    if [[ "$KSU_OPTION" == "y" ]]; then
        NAME="${version}_${MODEL}_UNOFFICIAL_KSU_${DATE}.zip"
    else
        NAME="${version}_${MODEL}_UNOFFICIAL_${DATE}.zip"
    fi

    zip -r "../$NAME" . || abort

    popd > /dev/null || abort
fi

# ============================================================
# FINISHED
# ============================================================

popd > /dev/null || exit 1

echo "-----------------------------------------------"
echo "Build finished successfully!"
echo "-----------------------------------------------"
