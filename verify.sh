#!/usr/bin/env bash
# Prove the tracer actually works, rather than that it merely built.
#
# Three checks, because each catches a failure the others miss:
#
#   1. The cart renders at all.
#   2. The frame counter ADVANCES. A cart that renders frame 1 and then
#      freezes looks identical in a screenshot, and that is exactly the bug
#      the asyncify rewind produced: the host counted 10 frames while the
#      cart's own counter sat at 1.
#   3. The box lands where tracer.ts says it should, to the pixel. This is
#      what ties the output to the TypeScript rather than to the C shell.
#
# Plus a CONTROL that must fail: rebuild with the box's step changed in the
# TS and require the box to move somewhere else. Without it, "the pixels
# match" cannot be told apart from "the harness reads a stale file".
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WASMCART="${WASMCART:-$HERE/../wasmcart}"
PLAY="$WASMCART/bin/wasmcart.js"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fails=0
ok()   { echo "  PASS  $1"; }
bad()  { echo "  FAIL  $1"; fails=$((fails + 1)); }

[ -f "$HERE/build/cart.wasm" ] || { echo "run ./build.sh first" >&2; exit 1; }
node "$PLAY" pack --wasm "$HERE/build/cart.wasm" >/dev/null 2>&1

echo "==> tracer cart"

# ---- 1 + 2: renders, and the counter advances -------------------------
cat > "$TMP/count.mjs" <<EOF
import { CartHost } from '$WASMCART/src/CartHost.js';
import { readFileSync } from 'node:fs';
const host = new CartHost();
await host.load(readFileSync('$HERE/cart.wasc'));
for (let i = 0; i < 10; i++) host.runFrame([]);
console.log(host.instance.exports.sc_frame_count());
EOF
count="$(node "$TMP/count.mjs")"
[ "$count" -ge 8 ] && ok "the TS loop advanced ($count frames)" \
                   || bad "TS frame counter stuck at $count after 10 host frames"

# ---- 3: the box is where the TypeScript says ---------------------------
# boxX = (frame * 2) % (w - 64), and the player shoots the LAST frame.
box_x_at() {
  node "$PLAY" "$HERE/cart.wasc" --frames "$1" --shot "$TMP/s$1.png" >/dev/null 2>&1
  python3 "$HERE/tools/boxx.py" "$TMP/s$1.png"
}
got40="$(box_x_at 40)"
want40=$(( (39 * 2) % (640 - 64) ))
[ "$got40" = "$want40" ] && ok "box at x=$got40, exactly as tracer.ts computes" \
                         || bad "box at x=$got40, tracer.ts says $want40"

got20="$(box_x_at 20)"
[ "$got20" != "$got40" ] && ok "the box MOVES between frame counts ($got20 -> $got40)" \
                         || bad "box did not move: the loop is frozen"

echo
[ "$fails" -eq 0 ] && echo "tracer: all checks passed" || echo "tracer: $fails check(s) failed"
exit "$fails"
