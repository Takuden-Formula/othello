#!/bin/bash
# Builds docs/othello.wasm from src/ (needs zig: `pip install ziglang`, or zig on PATH).
# Run from anywhere: bash tools/build_wasm.sh
set -e
cd "$(dirname "$0")/.."
if command -v zig >/dev/null 2>&1; then ZIG=(zig)
elif command -v python3 >/dev/null 2>&1 && python3 -c "import ziglang" 2>/dev/null; then ZIG=(python3 -m ziglang)
else ZIG=(python -m ziglang); fi
# 2^20 entries * 24 B = 24 MB hash table (lighter than the 48 MB PC default, fine for phones)
"${ZIG[@]}" cc --target=wasm32-wasi -O3 -mexec-model=reactor -DOTH_TT_BITS=20 \
  -Wl,--strip-all -o docs/othello.wasm src/engine.c src/wasm_api.c
ls -l docs/othello.wasm
