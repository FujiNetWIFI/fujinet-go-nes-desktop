#!/usr/bin/env python3
"""Render the desktop icon set from the shared FujiNet Go launcher art.

The artwork (data/icons/src/fujinet-go-nes-foreground.png) is the exact
same transparent FujiNet mark the other desktop apps use -- byte-identical,
copied from fujinet-go-intv-desktop's own copy -- composited over this
product's own background colour, so the whole family reads as one product
line while each target still gets a distinct badge colour.

One thing is specific to this target: the colours are the NES console's own
plastic. The background is the light grey of the console's shell (#C6C6C6)
and the mark is drawn in the dark grey of its front band and cartridge door
(#3C3C3C). The family mark is white discs and lines around a black centre
disc, so it is recoloured by brightness rather than inverted: white becomes
the dark grey and black the light grey (antialiased edges blend between
them), which leaves the centre disc reading as a hole in the band, as the
2600's inverted mark does.

The UI accent stays the NES logo's red (NESSESSION_ACCENT_RGB): a grey
highlight would not read as one.

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
FOREGROUND = ROOT / "data/icons/src/fujinet-go-nes-foreground.png"
OUTDIR = ROOT / "data/icons/hicolor"
ICNS_OUT = ROOT / "data/icons/fujinet-go-nes.icns"
ICO_OUT = ROOT / "frontends/windows/app.ico"

BACKGROUND = (0xC6, 0xC6, 0xC6, 0xFF)    # #C6C6C6, the console's light grey shell
MARK = (0x3C, 0x3C, 0x3C)                # #3C3C3C, its dark grey front band
MASTER = 1024                            # render big, downsample with LANCZOS
CORNER_RADIUS = 0.22                     # fraction of the edge
FOREGROUND_ZOOM = 1.18                   # Android's mask crops; compensate a little
SIZES = (16, 24, 32, 48, 64, 128, 192, 256, 512)
ICNS_SIZES = (32, 64, 128, 256, 512, 1024)  # every size macOS's icns TOC references


def render_master() -> Image.Image:
    art = Image.open(FOREGROUND).convert("RGBA")
    # Recolour by brightness (alpha untouched: it is the mark's shape):
    # white -> MARK, black -> BACKGROUND, edges in between.
    r, g, b, a = art.split()
    lum = Image.merge("RGB", (r, g, b)).convert("L")
    dark = Image.new("RGB", art.size, MARK)
    light = Image.new("RGB", art.size, BACKGROUND[:3])
    art = Image.composite(dark, light, lum).convert("RGBA")
    art.putalpha(a)

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
        out = OUTDIR / f"{size}x{size}" / "apps" / "fujinet-go-nes.png"
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
