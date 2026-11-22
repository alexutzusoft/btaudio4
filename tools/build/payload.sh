#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$script_dir/common.sh"

bta_init_paths "$script_dir"
bta_init_toolchain
bta_read_version

build="$BTA_ROOT/build/bta-app"
mkdir -p "$build"

"$BTA_PYTHON" - "$BTA_ROOT/src/payload/web.html" "$build/web_page.h" "$BTA_VER" <<'PY'
import sys
from pathlib import Path

html = Path(sys.argv[1]).read_text(encoding='utf-8').replace('__BTA_VERSION__', sys.argv[3]).encode('utf-8')
Path(sys.argv[2]).write_text('static const char web_page[] = {' + ','.join(map(str, html)) + ',0};', encoding='ascii')
PY

"$BTA_PYTHON" "$BTA_ROOT/src/assets/generate.py" --notification-icon-header "$build/notification_icon.h"

flags=(
    --target=x86_64-pc-freebsd12-elf
    -O2 -fPIE -ffreestanding -fno-builtin -fno-stack-protector
    -mno-red-zone -fno-asynchronous-unwind-tables
    -Wall -Wextra -Werror
    -I "$build"
)

objects=()
for name in launcher worker diag update; do
    obj="$build/$name.o"
    "$BTA_CLANG" "${flags[@]}" -c "$BTA_ROOT/src/payload/$name.c" -o "$obj"
    objects+=("$obj")
done

obj="$build/bt_accept_patch.o"
"$BTA_CLANG" "${flags[@]}" -mgeneral-regs-only -D_start=bta_apply_patch -c "$BTA_ROOT/src/payload/kernel/bt_accept_patch.c" -o "$obj"
objects+=("$obj")

for name in kresolve bt_devices; do
    obj="$build/$name.o"
    "$BTA_CLANG" "${flags[@]}" -mgeneral-regs-only -c "$BTA_ROOT/src/payload/kernel/$name.c" -o "$obj"
    objects+=("$obj")
done

script="$BTA_ROOT/src/payload/payload.ld"
"$BTA_LINKER" -T "$script" "${objects[@]}" -o "$build/BTA-Audio.debug.elf"
"$BTA_LINKER" --oformat=binary -T "$script" "${objects[@]}" -o "$BTA_ROOT/build/BTAudio4-$BTA_VER.bin"
echo "Built build/BTAudio4-$BTA_VER.bin"
