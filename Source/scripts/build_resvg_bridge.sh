#!/usr/bin/env bash
set -euo pipefail

project_root=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
bridge="$project_root/third_party/resvg_bridge"
cargo_bin=${CARGO:-cargo}
rustc_bin=${RUSTC:-rustc}

(
  cd "$bridge"
  if [[ $("$rustc_bin" --version) != rustc\ 1.98.0* ]]; then
    printf 'The pinned resvg bridge requires Rust 1.98.0\n' >&2
    exit 1
  fi
  "$cargo_bin" test --locked
  "$cargo_bin" build --locked --profile production
)

library="$bridge/target/production/libvove_resvg_bridge.a"
test -f "$library"
printf 'RESVG_BRIDGE_LIBRARY=%s\n' "$library"
printf 'RESVG_BRIDGE_BYTES=%s\n' "$(stat -c %s "$library")"
printf 'RESVG_BRIDGE_SHA256=%s\n' "$(sha256sum "$library" | cut -d' ' -f1)"
