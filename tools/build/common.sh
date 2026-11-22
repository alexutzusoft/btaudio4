#!/usr/bin/env bash

bta_init_paths() {
    local script_dir="$1"
    BTA_ROOT="$(cd "$script_dir/../.." && pwd)"
    BTA_WORKSPACE="$(dirname "$BTA_ROOT")"
}

bta_find_bin() {
    local env_val="$1"
    local local_path="$2"
    shift 2
    if [[ -n "$env_val" ]]; then
        printf '%s\n' "$env_val"
        return 0
    fi
    if [[ -x "$local_path" ]]; then
        printf '%s\n' "$local_path"
        return 0
    fi
    local name
    for name in "$@"; do
        if command -v "$name" >/dev/null 2>&1; then
            command -v "$name"
            return 0
        fi
    done
    echo "Missing required tool: $1" >&2
    return 1
}

bta_init_toolchain() {
    BTA_CLANG="$(bta_find_bin "${CLANG:-}" "$BTA_WORKSPACE/toolchain/llvm/bin/clang" clang)"
    BTA_LINKER="$(bta_find_bin "${LD_LLD:-}" "$BTA_WORKSPACE/toolchain/llvm/bin/ld.lld" ld.lld)"
    BTA_PYTHON="$(bta_find_bin "${PYTHON:-}" "" python3 python)"
}

bta_read_version() {
    local ver_file="$BTA_ROOT/src/common/version.h"
    BTA_VER="$(sed -nE 's/^#define[[:space:]]+BTA_VERSION[[:space:]]+"([^"]+)".*$/\1/p' "$ver_file" | tr -d '\r' | head -n 1)"
    if [[ -z "$BTA_VER" ]]; then
        echo "Could not parse BTA_VERSION from src/common/version.h" >&2
        return 1
    fi
}
