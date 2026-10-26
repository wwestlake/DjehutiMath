"""Regenerates the Djehuti Math branding derivatives from the approved original.

    python tools/make_branding.py

The approved artwork ("Djehuti MATH_ Golden Ibis Emblem.png", project root) is only read, never modified or moved.
Derivatives are square, proportion-preserving, Lanczos-resampled copies written to Resources/branding.
"""
import pathlib
import sys

from PIL import Image

root = pathlib.Path(__file__).resolve().parent.parent
source = root / "Djehuti MATH_ Golden Ibis Emblem.png"
out = root / "Resources" / "branding"
sizes = [16, 24, 32, 48, 64, 96, 128, 256, 512]

if not source.exists():
    sys.exit(f"approved artwork not found: {source}")
image = Image.open(source).convert("RGBA")
if image.width != image.height:
    sys.exit("the artwork is not square; refusing to distort it")
out.mkdir(parents=True, exist_ok=True)
for size in sizes:
    image.resize((size, size), Image.LANCZOS).save(out / f"djehuti-math-{size}.png", optimize=True)
image.resize((256, 256), Image.LANCZOS).save(
    out / "djehuti-math.ico", format="ICO", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
print("wrote", len(sizes), "PNG sizes and djehuti-math.ico to", out)
