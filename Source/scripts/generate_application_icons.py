"""Export the approved PDF-compatible Illustrator icons without redrawing them.

Build-time tools only: the existing pypdfium2 and Pillow development runtime.
Run with --source <approved .ai file>; --check verifies reproducibility without writes.
The source is opened read-only and its authored fills, gradients and paths are retained.
"""

import argparse
import hashlib
import io
from pathlib import Path

import pypdfium2 as pdfium
from PIL import Image


SOURCE_SHA256 = "cee718986a04bb5a1dc608932dbc9460bfef14563908c1726a9bdd9fd65a1215"
# Painted bounds in PDF points, including the light variant's 0.726-point outline.
BOUNDS = {
    "dark": (0.0, 0.3628, 98.5067, 98.8695),
    "light": (120.4389, 0.0, 219.671, 99.2321),
}
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def export_icons(source):
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("Source differs from the approved artwork; recheck the crop bounds.")

    result = {}
    icons = {}
    for theme, bounds in BOUNDS.items():
        with pdfium.PdfDocument(data) as document:
            if len(document) != 1:
                raise ValueError("Expected the approved single-page artwork.")
            page = document[0]
            page.set_cropbox(*bounds)
            scale = 2048 / max(page.get_size())
            bitmap = page.render(scale=scale, fill_color=(0, 0, 0, 0))
            try:
                rendered = bitmap.to_pil().convert("RGBA")
                icons[theme] = rendered.resize((512, 512), Image.Resampling.LANCZOS)
            finally:
                bitmap.close()
        buffer = io.BytesIO()
        icons[theme].save(buffer, format="PNG", optimize=True)
        result[f"resources/artwork/application-icon-{theme}.png"] = buffer.getvalue()

    result["packaging/linux/vo-ve.png"] = result["resources/artwork/application-icon-light.png"]
    buffer = io.BytesIO()
    icons["light"].save(buffer, format="ICO", sizes=[(size, size) for size in ICO_SIZES])
    result["packaging/windows/vo-ve.ico"] = buffer.getvalue()
    result["packaging/windows/vo-ve-light.ico"] = buffer.getvalue()
    buffer = io.BytesIO()
    icons["dark"].save(buffer, format="ICO", sizes=[(size, size) for size in ICO_SIZES])
    result["packaging/windows/vo-ve-dark.ico"] = buffer.getvalue()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    for relative, data in export_icons(args.source).items():
        path = args.root / relative
        if args.check:
            if not path.is_file() or path.read_bytes() != data:
                raise ValueError(f"Generated asset is out of date: {relative}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        print(f"{relative}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
