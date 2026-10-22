#!/usr/bin/env python3
"""Export Glint's SVG, PNG, and ICO assets from one set of vector coordinates.

Requires Python 3.10+ and Pillow. Run from any directory:
    python scripts/generate-brand-assets.py

The exports contain no timestamps or external assets. Running this script twice
with the same Pillow version produces identical files.
"""

from __future__ import annotations

from io import BytesIO
from pathlib import Path
import struct

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent.parent
BLUE = "#2c61bd"
ICE = "#dce8ff"
INK = "#223043"
MUTED = "#53657b"
DOT = "#a9c6ff"
STAR = ((50, 8), (60.8, 39.2), (92, 50), (60.8, 60.8),
        (50, 92), (39.2, 60.8), (8, 50), (39.2, 39.2))
INNER = ((50, 27.5), (56.2, 43.8), (72.5, 50), (56.2, 56.2),
         (50, 72.5), (43.8, 56.2), (27.5, 50), (43.8, 43.8))
ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def mark_svg() -> str:
    return f'''<rect width="100" height="100" rx="24" fill="{BLUE}"/>
  <path d="M{' L'.join(f'{x:g} {y:g}' for x, y in STAR)} Z" fill="#fff"/>
  <path d="M{' L'.join(f'{x:g} {y:g}' for x, y in INNER)} Z" fill="{ICE}"/>
  <circle cx="75" cy="25" r="7" fill="{DOT}"/>'''


def write_text(relative: str, content: str) -> None:
    path = ROOT / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def render_mark(size: int) -> Image.Image:
    # Supersampling keeps the small tray and toolbar exports crisp without a
    # runtime graphics library. The icon is transparent outside the blue tile.
    scale = 8
    ratio = size * scale / 100
    image = Image.new("RGBA", (size * scale, size * scale))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((0, 0, image.width - 1, image.height - 1),
                           radius=24 * ratio, fill=BLUE)
    draw.polygon([(x * ratio, y * ratio) for x, y in STAR], fill="white")
    draw.polygon([(x * ratio, y * ratio) for x, y in INNER], fill=ICE)
    draw.ellipse((68 * ratio, 18 * ratio, 82 * ratio, 32 * ratio), fill=DOT)
    return image.resize((size, size), Image.Resampling.LANCZOS)


def png_bytes(image: Image.Image) -> bytes:
    output = BytesIO()
    image.save(output, format="PNG", optimize=False, compress_level=9)
    return output.getvalue()


def write_png(relative: str, size: int) -> None:
    path = ROOT / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png_bytes(render_mark(size)))


def write_ico() -> None:
    # PNG-compressed 32-bit entries are supported by Windows Vista and later.
    # Writing the directory ourselves preserves every requested intermediate
    # DPI size, including 20, 24, and 40 px, without resampling at runtime.
    images = [png_bytes(render_mark(size)) for size in ICON_SIZES]
    offset = 6 + 16 * len(images)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    for size, image in zip(ICON_SIZES, images):
        dimension = size if size < 256 else 0
        directory.extend(struct.pack("<BBBBHHII", dimension, dimension, 0, 0,
                                     1, 32, len(image), offset))
        offset += len(image)
    (ROOT / "assets/glint.ico").write_bytes(bytes(directory) + b"".join(images))


def main() -> None:
    mark = mark_svg()
    write_text("assets/glint-mark.svg", f'''<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 100 100" role="img" aria-labelledby="title desc">
  <title id="title">Glint</title>
  <desc id="desc">A white glint and small ice-blue dot on a rounded blue square.</desc>
  {mark}
</svg>
''')
    write_text("docs/assets/glint-logo.svg", f'''<svg xmlns="http://www.w3.org/2000/svg" width="460" height="120" viewBox="0 0 460 120" role="img" aria-labelledby="title desc">
  <title id="title">Glint</title>
  <desc id="desc">Glint logo and wordmark.</desc>
  <g transform="translate(10 10)">{mark}</g>
  <text x="140" y="87" fill="{INK}" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="82" font-weight="600" letter-spacing="-3">Glint</text>
</svg>
''')
    write_text("docs/assets/glint-banner.svg", f'''<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="320" viewBox="0 0 1200 320" role="img" aria-labelledby="title desc">
  <title id="title">Glint — Find your tab. Keep your flow.</title>
  <desc id="desc">Glint is a lightweight native Windows search for Chrome and Edge tabs.</desc>
  <rect width="1200" height="320" rx="28" fill="#f5f8fd"/>
  <path d="M1050 36 L1070 94 L1128 114 L1070 134 L1050 192 L1030 134 L972 114 L1030 94 Z" fill="#e8effc"/>
  <circle cx="1144" cy="235" r="8" fill="#d9e5fb"/>
  <circle cx="996" cy="246" r="4" fill="#d9e5fb"/>
  <g transform="translate(78 68) scale(1.84)">{mark}</g>
  <text x="310" y="152" fill="{INK}" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="82" font-weight="600" letter-spacing="-3">Glint</text>
  <text x="314" y="203" fill="{MUTED}" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="27">Find your tab. Keep your flow.</text>
  <text x="315" y="246" fill="{BLUE}" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="15" font-weight="600" letter-spacing="1.5">WINDOWS · CHROME · EDGE</text>
</svg>
''')
    write_png("assets/glint.png", 512)
    write_ico()
    for size in (16, 32, 48, 128):
        write_png(f"extension/icon-{size}.png", size)
    print("Exported 3 SVGs, 5 PNGs, and a 9-size ICO from the Glint vector geometry.")


if __name__ == "__main__":
    main()
