#!/usr/bin/env bash
# Build the text-input cart: wasmcart 0.16's text ABI, bridged to
# scriptc-game's polled event queue by src/cart_input.cpp.
#
# The interesting part is not Canvas -- build-canvas.sh already proved that
# tier relinks unchanged. It is that the host and the game disagree about the
# SHAPE of text input: the host delivers it by callback (wc_on_text), the game
# polls an event queue (EV_TEXT). Everything else about this build is
# build-canvas.sh, plus cart_input.o and the wc_on_text export.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SG="${SCRIPTC_GAME:-$HERE/../scriptc-game}"
SKIA="${WASMCART_SKIA:-$HERE/../wasmcart-skia/out}"
SKIA_HDRS="${SKIA_HEADERS:-$HERE/../napi-canvas/skia}"
SCRIPTC_REPO="${SCRIPTC_REPO:-$HERE/../scriptc}"
SCRIPTC_BIN="${SCRIPTC_BIN:-$SCRIPTC_REPO/packages/cli/dist/main.js}"
RT="$SCRIPTC_REPO/packages/runtime"
OUT="$HERE/build"
OBJ="$OUT/obj-text"

command -v emcc >/dev/null 2>&1 || { echo "build-text.sh: emcc not on PATH" >&2; exit 1; }
[ -f "$SKIA/libskiac.a" ] || { echo "build-text.sh: no libskiac.a in $SKIA" >&2; exit 1; }
[ -d "$SG/web/canvas" ]   || { echo "build-text.sh: scriptc-game not at $SG" >&2; exit 1; }

mkdir -p "$OBJ"

CXXF="-O2 -std=c++20 -fno-exceptions -fno-rtti -DSK_RELEASE -DSG_NO_FONT_PATH -I$SG/shim -I$SKIA/include -I$SKIA_HDRS"

# ---- 1. the FFI manifest ---------------------------------------------
#
# gen-ffi walks the TS import graph and needs the C signatures for every
# symbol it finds. Ours live outside scriptc-game, hence SG_EXTRA_SHIM.
echo "==> ffi manifest"
( cd "$SG" && SG_EXTRA_SHIM="$HERE/src/text_cart.c:$HERE/src/cart_core.cpp:$HERE/src/cart_input.cpp" \
  node codegen/gen-ffi.js linux-x86_64 "$HERE/src/text_tracer.ts" >/dev/null )

# Strip the native link inputs. scriptc runs a real link to produce the IR,
# and a cart has no SDL2/GL/fontconfig to link against; the wasm link in step
# 6 supplies the archives itself. Without this scriptc stops at "unable to
# find dynamic system library 'SDL2'" and never writes the .ll.
python3 - "$SG/ffi/core.ffi.json" "$OUT/cart.ffi.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["libraries"] = []
d["system_libraries"] = []
json.dump(d, open(sys.argv[2], "w"), indent=2)
PYEOF

# ---- 2. TypeScript -> LLVM IR ----------------------------------------
#
# SCRIPTC_TARGET is a 32-BIT triple, and that is the whole point: it makes
# the backend emit i32 for size_t, matching wasm32. x86-linux-gnu rather than
# wasm32-wasi because scriptc's own C runtime does not compile under WASI
# (~45 POSIX errors) and the IR is never written; x86 gets far enough.
#
# The native link at the end fails by design (our shim symbols are wasm
# objects); the .ll is written first and is what we want.
echo "==> scriptc: text_tracer.ts -> IR"
SCRIPTC_CC=zigcc SCRIPTC_TARGET=x86-linux-gnu \
  node "$SCRIPTC_BIN" build "$HERE/src/text_tracer.ts" \
    --ffi "$OUT/cart.ffi.json" -o "$OUT/text_native" >/dev/null 2>&1 || true
[ -f "$OUT/text_tracer.ll" ] || { echo "build-text.sh: no IR produced" >&2; exit 1; }

echo "==> emcc: IR -> wasm object"
sed 's/^define i32 @main(/define i32 @scriptc_main(/' \
    "$OUT/text_tracer.ll" > "$OUT/text_cart.ll"
emcc -O2 -c "$OUT/text_cart.ll" -o "$OBJ/tracer.o" 2>&1 \
  | grep -v "overriding the module target" || true

# ---- 3. scriptc runtime ----------------------------------------------
echo "==> emcc: scriptc runtime"
cat > "$OBJ/a4r_shim.h" <<'EOF'
#ifndef SC_A4R_SHIM_H
#define SC_A4R_SHIM_H
#include <unistd.h>
#include <stddef.h>
static inline void arc4random_buf(void *buf, size_t n) {
  unsigned char *p = (unsigned char *)buf;
  while (n > 0) { size_t c = n > 256 ? 256 : n; getentropy(p, c); p += c; n -= c; }
}
#endif
EOF
# scr_regex.c pulls lre_* out of quickjs-ng's libregexp/libunicode.
QJS="$RT/vendor/quickjs-ng"
for q in libregexp libunicode; do
  emcc -O2 -c "$QJS/$q.c" -I"$QJS" -o "$OBJ/$q.o" 2>/dev/null
done

rt_objs=("$OBJ/libregexp.o" "$OBJ/libunicode.o")
for f in "$RT"/src/*.c; do
  b="$(basename "$f" .c)"
  case "$b" in scr_http|scr_tls|scr_inspect_island) continue;; esac
  emcc -O2 -c "$f" -I"$RT/include" -I"$RT/src" -I"$QJS" -include "$OBJ/a4r_shim.h" -o "$OBJ/$b.o" 2>/dev/null
  rt_objs+=("$OBJ/$b.o")
done

# ---- 4. scriptc-game's skia shim, UNMODIFIED -------------------------
#
# The whole point: these are scriptc-game's files, compiled for wasm with no
# edits, against a different libskiac.a.
echo "==> emcc: scriptc-game skia shim (unmodified)"
em++ $CXXF -c "$SG/shim/sg_skia_gen.cpp"   -o "$OBJ/sg_skia_gen.o"
em++ $CXXF -c "$SG/shim/sg_skia_extra.cpp" -o "$OBJ/sg_skia_extra.o"
emcc -O2 -c "$SG/shim/sg_tables.c" -I"$SG/shim" -o "$OBJ/sg_tables.o"

# ---- 5. cart shell ----------------------------------------------------
echo "==> emcc: cart shell"
em++ $CXXF -c "$HERE/src/cart_core.cpp" -o "$OBJ/cart_core.o"
emcc -O2 -c "$HERE/src/text_cart.c" -I"$HERE/src" -o "$OBJ/text_cart.o"
# The text adapter. Without it sg_poll_event is auto-stubbed to -1 by the
# host and input.pump() spins forever, which looks like a hang during load
# rather than like missing input.
em++ -O2 -std=c++20 -fno-exceptions -fno-rtti -c "$HERE/src/cart_input.cpp" \
  -I"$HERE/src" -o "$OBJ/cart_input.o"

# ---- 6. link ----------------------------------------------------------
#
# Skia's archives must come in dependency order, and the whole set is needed:
# a partial link produced an out-of-bounds write on the first Canvas call
# rather than an undefined symbol (see wasmcart-jsgame/REBUILD_STATUS.md).
echo "==> emcc: link text.wasm"
emcc -O2 "$OBJ/text_cart.o" "$OBJ/cart_core.o" "$OBJ/cart_input.o" "$OBJ/tracer.o" \
  "$OBJ/sg_skia_gen.o" "$OBJ/sg_skia_extra.o" "$OBJ/sg_tables.o" \
  "${rt_objs[@]}" \
  "$SKIA/libskiac.a" "$SKIA/libskia.a" "$SKIA/libskshaper.a" \
  "$SKIA/libskparagraph.a" "$SKIA/libskunicode_core.a" "$SKIA/libskunicode_icu.a" \
  "$SKIA/libharfbuzz.a" "$SKIA/libicu.a" "$SKIA/libfreetype2.a" \
  "$SKIA/libpng.a" "$SKIA/libjpeg.a" "$SKIA/libwebp.a" "$SKIA/libwuffs.a" \
  "$SKIA/libzlib.a" "$SKIA/libskcms.a" \
  -o "$OUT/text-pre.wasm" \
  -sSTANDALONE_WASM -sERROR_ON_UNDEFINED_SYMBOLS=0 \
  -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=134217728 -sSTACK_SIZE=4194304 \
  --no-entry \
  -sEXPORTED_FUNCTIONS='["_wc_get_info","_wc_init","_wc_render","_wc_yield_buffer","_sc_frame_count","_wc_on_text","_sg_cart_text_calls"]'

echo "==> wasm-opt: asyncify around wc_frame_yield"
WASM_OPT="${WASM_OPT:-$(dirname "$(command -v emcc)")/../bin/wasm-opt}"
[ -x "$WASM_OPT" ] || WASM_OPT=wasm-opt
# --enable-threads is about VALIDATION, not concurrency. Current Skia emits
# atomic ops (7146 of them in libskia.a; a May 2026 build had none), and
# wasm-opt refuses to validate a module containing them unless the feature is
# enabled -- "Atomic operations require threads [--enable-threads]". The cart
# is still single-threaded: nothing here spawns a worker, and the atomics are
# uncontended. Without this the asyncify pass fails outright and no cart is
# produced.
"$WASM_OPT" --asyncify \
  --pass-arg=asyncify-imports@env.wc_frame_yield \
  --enable-nontrapping-float-to-int --enable-bulk-memory --enable-sign-ext \
  --enable-mutable-globals --enable-simd --enable-threads \
  -O2 "$OUT/text-pre.wasm" -o "$OUT/text.wasm"

# Pack with assets: the font is loaded through the host asset API rather
# than embedded, since a cart has no filesystem for a path-based register.
node "$HERE/../wasmcart/bin/wasmcart.js" pack \
  --wasm "$OUT/text.wasm" --assets "$HERE/assets" \
  --output "$HERE/text.wasc" >/dev/null

ls -la "$OUT/text.wasm" "$HERE/text.wasc"
echo "built $HERE/text.wasc"
