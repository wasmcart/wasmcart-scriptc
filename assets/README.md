# Cart assets

Loaded through the host's asset API (`wc_asset_size` / `wc_load_asset`), not
from a filesystem: a cart has none, which is why `cart_core.cpp` registers
fonts from BYTES rather than through scriptc-game's path-based
`sg_font_register`.

| File | Role | License |
| --- | --- | --- |
| `DejaVuSans.ttf` | The tracer's typeface | Bitstream Vera (redistributable) |
