#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$script_dir/common.sh"

bta_init_paths "$script_dir"
bta_init_toolchain
bta_read_version

sdk="${OO_PS4_TOOLCHAIN:-}"
if [[ -z "$sdk" ]]; then
    for candidate in \
        "$BTA_WORKSPACE/toolchain/OpenOrbis/PS4Toolchain" \
        "/opt/openorbis/OpenOrbis/PS4Toolchain" \
        "/opt/openorbis/PS4Toolchain" \
        "/usr/local/OpenOrbis/PS4Toolchain"; do
        if [[ -f "$candidate/link.x" && -x "$candidate/bin/linux/PkgTool.Core" ]]; then
            sdk="$candidate"
            break
        fi
    done
fi
if [[ -z "$sdk" || ! -f "$sdk/link.x" || ! -x "$sdk/bin/linux/PkgTool.Core" ]]; then
    echo "Could not locate Linux OpenOrbis PS4Toolchain (set OO_PS4_TOOLCHAIN)." >&2
    exit 1
fi

bin="$sdk/bin/linux"
out="$BTA_ROOT/build/native"
pkg="$out/package"

IFS='.' read -r ver_major ver_minor ver_patch <<<"$BTA_VER"
if [[ -n "${ver_patch:-}" ]]; then
    minor=$((10#$ver_minor * 10 + 10#$ver_patch))
elif [[ "${#ver_minor}" -eq 1 ]]; then
    minor=$((10#$ver_minor * 10))
else
    minor=$((10#$ver_minor))
fi
sfo_ver=$(printf '%02d.%02d' "$((10#$ver_major))" "$minor")

payload_bin="$BTA_ROOT/build/BTAudio4-$BTA_VER.bin"
if [[ ! -f "$payload_bin" ]]; then
    echo "Missing build/BTAudio4-$BTA_VER.bin; run tools/build/payload.sh first." >&2
    exit 1
fi

export DOTNET_ROLL_FORWARD=Major
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="${DOTNET_SYSTEM_GLOBALIZATION_INVARIANT:-1}"
export OO_PS4_TOOLCHAIN="$sdk"

"$BTA_PYTHON" "$BTA_ROOT/src/assets/generate.py"
mkdir -p "$pkg/sce_sys/about" "$pkg/sce_module" "$pkg/assets"

"$BTA_CLANG" --target=x86_64-pc-freebsd12-elf -O2 -fPIC -funwind-tables \
    -isysroot "$sdk" -isystem "$sdk/include" -I "$out" \
    -Wall -Wextra -Wno-misleading-indentation \
    -c "$BTA_ROOT/src/app/main.c" -o "$out/main.o"

"$BTA_LINKER" "$out/main.o" "$sdk/lib/crt1.o" -m elf_x86_64 -pie \
    --script "$sdk/link.x" --eh-frame-hdr -L "$sdk/lib" \
    -lc -lkernel -lSceVideoOut -lScePad -lSceUserService -lSceSystemService \
    -lSceNet -lSceSsl -lSceHttp -lSceSysmodule \
    -o "$out/BluetoothAudio.elf"

# Relative paths: create-fself records the -in path inside eboot.bin, so an absolute one would embed the build machine's folder names.
(
    cd "$out"
    "$bin/create-fself" -in=BluetoothAudio.elf -out=BluetoothAudio.oelf --eboot package/eboot.bin --paid 0x3800000000000011
)

cp -f "$out/icon0.png" "$pkg/sce_sys/icon0.png"
cp -f "$out/pic1.png" "$pkg/sce_sys/pic1.png"
cp -f "$sdk/samples/hello_world/sce_sys/about/right.sprx" "$pkg/sce_sys/about/right.sprx"
for module in libc.prx libSceFios2.prx; do
    cp -f "$sdk/samples/hello_world/sce_module/$module" "$pkg/sce_module/$module"
done
cp -f "$payload_bin" "$pkg/assets/BTA-Audio.bin"

sfo="$pkg/sce_sys/param.sfo"
tool="$bin/PkgTool.Core"
"$tool" sfo_new "$sfo"

# CATEGORY 'gde' (Non-Game Mini App) and ATTRIBUTE 32 (0x20) let PS4 suspend the app alongside an active game.
entries=(
    "APP_TYPE:Integer:4:1"
    "APP_VER:Utf8:8:$sfo_ver"
    "ATTRIBUTE:Integer:4:32"
    "CATEGORY:Utf8:4:gde"
    "CONTENT_ID:Utf8:48:IV0000-BTAD00001_00-BTAUDIO000000001"
    "DOWNLOAD_DATA_SIZE:Integer:4:0"
    "SYSTEM_VER:Integer:4:0"
    "TITLE:Utf8:128:BTAudio4"
    "TITLE_ID:Utf8:12:BTAD00001"
    "VERSION:Utf8:8:$sfo_ver"
)
for entry in "${entries[@]}"; do
    IFS=':' read -r key type maxsize value <<<"$entry"
    "$tool" sfo_setentry "$sfo" "$key" --type "$type" --maxsize "$maxsize" --value "$value"
done

(
    cd "$pkg"
    "$bin/create-gp4" -out package.gp4 --content-id=IV0000-BTAD00001_00-BTAUDIO000000001 \
        --files 'eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx assets/BTA-Audio.bin'
    "$tool" pkg_build package.gp4 "$out"
)

cp -f "$out/IV0000-BTAD00001_00-BTAUDIO000000001.pkg" "$BTA_ROOT/build/BTAudio4-$BTA_VER.pkg"
"$tool" pkg_validate "$BTA_ROOT/build/BTAudio4-$BTA_VER.pkg"
echo "Built and validated build/BTAudio4-$BTA_VER.pkg"
