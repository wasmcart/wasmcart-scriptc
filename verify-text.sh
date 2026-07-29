#!/usr/bin/env bash
# Prove text input reaches the GAME, not merely the cart.
#
# The path under test has four hops, and a break in any of them looks the
# same from outside (a text field that stays empty):
#
#   host.textInput("a")  ->  wc_on_text        (host -> cart)
#   wc_on_text           ->  g_text buffer     (cart_input.cpp)
#   sg_poll_event        ->  EV_TEXT           (the callback/queue adapter)
#   input.textEvents()   ->  the drawn string  (scriptc-game, unmodified)
#
# So the checks below separate them. `sg_cart_text_calls` reads hop 1
# directly, and the drawn pixels read hop 4; a divergence between the two
# localises the break to the adapter in between.
#
# Two controls that MUST fail, because "the field has text in it" is exactly
# what a harness that reads a stale PNG also reports:
#   - a run with no text typed must leave the field empty
#   - text sent BEFORE the cart enables text input must be dropped
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WASMCART="${WASMCART:-$HERE/../wasmcart}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fails=0
ok()  { echo "  PASS  $1"; }
bad() { echo "  FAIL  $1"; fails=$((fails + 1)); }

[ -f "$HERE/text.wasc" ] || { echo "run ./build-text.sh first" >&2; exit 1; }

echo "==> text cart (wasmcart 0.16 text ABI)"

# The harness. `type` is a JSON array of [frame, string] pairs, so a test can
# place a delivery before or after the cart asks for text.
cat > "$TMP/run.mjs" <<'EOF'
import { CartHost } from process.env.WC_SRC + '/CartHost.js';
EOF
cat > "$TMP/run.mjs" <<EOF
import { CartHost } from '$WASMCART/src/CartHost.js';
import { readFileSync, writeFileSync } from 'node:fs';

const frames = Number(process.argv[2]);
const script = JSON.parse(process.argv[3]);   // [[frame, "text"], ...]
const out    = process.argv[4];

const host = new CartHost();
await host.load(readFileSync('$HERE/text.wasc'));

let activeAt = -1;
for (let f = 0; f < frames; f++) {
  for (const [at, s] of script) if (at === f) host.textInput(s);
  host.runFrame([]);
  if (activeAt < 0 && host.textInputActive) activeAt = f;
}

// The framebuffer, so the check can read what the GAME drew rather than
// trusting the cart's own counters.
const info = host.instance.exports.wc_get_info();
const mem  = new DataView(host.instance.exports.memory.buffer);
const fbp  = mem.getUint32(info + 12, true);
const w = mem.getUint32(info + 4, true), h = mem.getUint32(info + 8, true);
const px = new Uint32Array(host.instance.exports.memory.buffer, fbp, w * h);

// Count non-background pixels inside the text field box (24,52 - w-24,96),
// where text_tracer.ts draws the string. The field's own fill is #1c2231, so
// anything else in there is a glyph or the caret.
let ink = 0;
for (let y = 56; y < 92; y++)
  for (let x = 36; x < w - 28; x++) {
    const c = px[y * w + x] & 0xffffff;
    if (c !== 0x1c2231) ink++;
  }

writeFileSync(out, JSON.stringify({
  calls: host.instance.exports.sg_cart_text_calls?.() ?? -1,
  frames: host.instance.exports.sc_frame_count(),
  activeAt, ink,
}));
EOF

run() {  # run <frames> <script-json> -> sets $calls $ink $activeAt $rframes
  node "$TMP/run.mjs" "$1" "$2" "$TMP/r.json" >/dev/null
  calls=$(python3 -c "import json;print(json.load(open('$TMP/r.json'))['calls'])")
  ink=$(python3 -c "import json;print(json.load(open('$TMP/r.json'))['ink'])")
  activeAt=$(python3 -c "import json;print(json.load(open('$TMP/r.json'))['activeAt'])")
  rframes=$(python3 -c "import json;print(json.load(open('$TMP/r.json'))['frames'])")
}

# ---- 0: the cart asks for text ---------------------------------------
# text_tracer.ts calls input.textInput(true) before its first frame. If that
# never reaches the host, every later delivery is dropped and all the checks
# below would fail for the wrong reason.
run 20 '[]'
[ "$activeAt" -ge 0 ] && ok "cart enabled text input (host saw it at frame $activeAt)" \
                      || bad "cart never called wc_text_input_begin"

# ---- CONTROL: no text typed -> the field must be empty ---------------
# The caret blinks, so a small amount of ink is expected even here. This
# records the floor the real runs have to clear.
empty_ink="$ink"
[ "$calls" -eq 0 ] && ok "control: no text sent, no wc_on_text calls" \
                   || bad "control: $calls wc_on_text calls with nothing sent"

# ---- 1: text reaches the cart ----------------------------------------
run 40 '[[5,"hello"],[12," "],[15,"world"]]'
[ "$calls" -eq 3 ] && ok "3 deliveries reached wc_on_text" \
                   || bad "expected 3 wc_on_text calls, got $calls"

# ---- 2: text reaches the GAME ----------------------------------------
# This is the hop the adapter owns. The counters above can be perfect while
# sg_poll_event never reports EV_TEXT, in which case the field stays empty.
typed_ink="$ink"
[ "$typed_ink" -gt $(( empty_ink + 200 )) ] \
  && ok "the game drew the typed text ($typed_ink ink vs $empty_ink empty)" \
  || bad "field ink $typed_ink vs empty $empty_ink: text never reached the game"

# ---- 3: more text draws more --------------------------------------
# Guards against a one-shot bug: a buffer that latches the first delivery and
# ignores the rest passes check 2 and fails here.
run 40 '[[5,"hello world this is longer"]]'
long_ink="$ink"
[ "$long_ink" -gt "$typed_ink" ] \
  && ok "a longer string draws more ink ($long_ink > $typed_ink)" \
  || bad "longer string drew $long_ink, not more than $typed_ink"

# ---- CONTROL: delivery before the cart asks must be dropped ----------
# The host gates on _textActive. Frame 0's delivery happens before the cart's
# first render, so it must not arrive. If this "passes" with calls>0, the
# gate is not working and check 1 proves less than it appears to.
run 40 '[[0,"ghost"]]'
[ "$calls" -eq 0 ] && ok "control: text before wc_text_input_begin was dropped" \
                   || bad "control: $calls calls delivered before the cart asked"

echo
[ "$fails" -eq 0 ] && { echo "text: all checks passed"; exit 0; }
echo "text: $fails check(s) failed"; exit 1
