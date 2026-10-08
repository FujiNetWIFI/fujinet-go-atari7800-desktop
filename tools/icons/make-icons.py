#!/usr/bin/env python3
"""Render the desktop icon set from the shared FujiNet Go launcher art.

The artwork (data/icons/src/fujinet-go-atari7800-foreground.png) is the exact
same transparent FujiNet mark the other desktop apps use -- byte-identical
to the siblings' copies -- composited over this product's own background
colour, so the whole family reads as one product line while each target
still gets a distinct badge colour.

Two things are specific to this target:

  * Background is #F0932F, a slightly darker orange than the Atari 2600
    app's #FFA645 (per the user's request for this app) -- the same colour
    the frontends use for UI highlights (A7800SESSION_ACCENT_RGB in
    core/include/a7800session.h).
  * The mark is INVERTED before compositing, as on the 2600's icon. The
    family art is three white discs and white connecting lines around one
    black centre disc; on an orange ground that would wash out, so the RGB
    channels are inverted (alpha untouched): the majority of the mark comes
    out black and the centre disc white.

Everything else about the composite (rounded-square mask, corner radius,
foreground zoom, output sizes) matches the sibling repos' own
tools/icons/make-icons.py exactly.

The results are committed (data/icons/hicolor/..., data/icons/*.icns,
frontends/windows/app.ico) so building the project needs no image tooling;
re-run this only when the artwork or background colour changes:

    python3 tools/icons/make-icons.py
"""

import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
FOREGROUND = ROOT / "data/icons/src/fujinet-go-atari7800-foreground.png"
OUTDIR = ROOT / "data/icons/hicolor"
ICNS_OUT = ROOT / "data/icons/fujinet-go-atari7800.icns"
ICO_OUT = ROOT / "frontends/windows/app.ico"

BACKGROUND = (0xF0, 0x93, 0x2F, 0xFF)    # #F0932F, the app's accent orange
INVERT_FOREGROUND = True                 # black mark, white centre (see above)
MASTER = 1024                            # render big, downsample with LANCZOS
CORNER_RADIUS = 0.22                     # fraction of the edge
FOREGROUND_ZOOM = 1.18                   # Android's mask crops; compensate a little
SIZES = (16, 24, 32, 48, 64, 128, 192, 256, 512)
ICNS_SIZES = (32, 64, 128, 256, 512, 1024)  # every size macOS's icns TOC references


def render_master() -> Image.Image:
    art = Image.open(FOREGROUND).convert("RGBA")
    if INVERT_FOREGROUND:
        # Invert colour only; the alpha channel is the mark's shape and must
        # not change, or the transparent field would become opaque white.
        r, g, b, a = art.split()
        art = Image.merge("RGBA", (ImageChops.invert(r), ImageChops.invert(g),
                                   ImageChops.invert(b), a))

    # Rounded-square background on a transparent canvas, drawn at 4x and
    # downsampled so the corners are antialiased.
    scale = 4
    big = Image.new("RGBA", (MASTER * scale, MASTER * scale), (0, 0, 0, 0))
    ImageDraw.Draw(big).rounded_rectangle(
        (0, 0, MASTER * scale - 1, MASTER * scale - 1),
        radius=int(MASTER * scale * CORNER_RADIUS),
        fill=BACKGROUND,
    )
    icon = big.resize((MASTER, MASTER), Image.LANCZOS)

    art_size = int(MASTER * FOREGROUND_ZOOM)
    art = art.resize((art_size, art_size), Image.LANCZOS)
    offset = (MASTER - art_size) // 2
    overlay = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    overlay.paste(art, (offset, offset), art)

    # Keep the foreground inside the rounded silhouette.
    composed = Image.alpha_composite(icon, overlay)
    composed.putalpha(Image.composite(composed.getchannel("A"),
                                      Image.new("L", (MASTER, MASTER), 0),
                                      icon.getchannel("A")))
    return composed


def main() -> int:
    if not FOREGROUND.exists():
        print(f"missing artwork: {FOREGROUND}", file=sys.stderr)
        return 1

    master = render_master()
    for size in SIZES:
        out = OUTDIR / f"{size}x{size}" / "apps" / "fujinet-go-atari7800.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        master.resize((size, size), Image.LANCZOS).save(out, optimize=True)
        print(f"wrote {out.relative_to(ROOT)}")

    # Pillow's ICNS writer works on any platform (no iconutil needed): it
    # just packs PNGs into the icns TOC. Pass every non-master size in
    # explicitly, LANCZOS-downsampled from the 1024 master like the hicolor
    # set above, so nothing gets a blurry re-resize from a smaller source.
    variants = [master.resize((size, size), Image.LANCZOS)
                for size in ICNS_SIZES if size != master.width]
    master.save(ICNS_OUT, format="ICNS", append_images=variants)
    print(f"wrote {ICNS_OUT.relative_to(ROOT)}")

    # The Windows .rc-embedded icon (frontends/windows/resource.rc's own
    # IDI_APPICON). Pillow's ICO writer packs whichever sizes are passed as
    # the `sizes` kwarg, resampling from `master` itself -- matching the
    # sibling repos' own app.ico (16x16 and 32x32, both 32bpp).
    ICO_OUT.parent.mkdir(parents=True, exist_ok=True)
    master.save(ICO_OUT, format="ICO", sizes=[(16, 16), (32, 32)])
    print(f"wrote {ICO_OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
