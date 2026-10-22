# Glint visual identity

Glint's mark combines a four-point glint with a small satellite dot. The blue
tile is easy to find in the Windows tray and browser toolbar; the open shape
keeps the identity light at small sizes. The same mark appears in the desktop
app, browser extension, and README artwork.

## Palette

| Use | Color |
| --- | --- |
| Brand blue, primary actions | `#2c61bd` |
| Ice accent inside the mark | `#dce8ff` |
| Main text | `#223043` |
| Supporting text | `#53657b` |
| Page background | `#f5f8fd` |
| Borders | `#d7e1ec` |
| Selected and focus surfaces | `#eaf1ff` |
| Connected state | `#237c56` |

Use white surfaces, quiet borders, clear spacing, and system fonts. Keep native
Windows controls and keyboard navigation. The visual treatment adds no web UI
framework, external fonts, runtime image processing, or continuous animation.

## Asset sources and exports

- `assets/glint-mark.svg` is the standalone vector mark.
- `assets/glint.png` is a 512 px transparent export.
- `assets/glint.ico` contains 16, 20, 24, 32, 40, 48, 64, 128, and 256 px images
  for Windows Explorer, title bars, and the tray. It is embedded as resource 201.
- `docs/assets/glint-logo.svg` is the horizontal logo and wordmark.
- `docs/assets/glint-banner.svg` is the README artwork. It is included with the
  packaged documentation, so its relative README link also works in `dist/`.
- `extension/icon-16.png`, `icon-32.png`, `icon-48.png`, and `icon-128.png` are
  browser toolbar and extension-management icons. They are embedded in the EXE,
  extracted beside the five extension runtime files, and published at the stable
  `dist/browser-extension/` path.

All assets are exported by `scripts/generate-brand-assets.py`. The script keeps
one set of coordinates for the SVG and raster versions and uses Pillow only
during asset generation. Run it with Python 3.10+ and Pillow:

```powershell
python scripts/generate-brand-assets.py
```

Exports contain no timestamps or external resources. With the same Pillow
version, repeated runs produce identical bytes. The SVG viewBox is 100 × 100;
the blue tile has a 24-unit corner radius. Keep the mark's proportions, glint,
and dot together when scaling it. Do not add strokes, shadows, or text inside
small program and toolbar icons.
