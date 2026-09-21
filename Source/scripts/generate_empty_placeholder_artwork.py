"""Export authored empty-file and empty-folder artwork from PDF-compatible AI files.

Build-time tools only: pypdfium2 and Pillow. The output keeps the four authored theme
variants transparent. Folder inks are blended 20 percent toward each theme background,
which is the approved two-step reduction in visual intensity.
"""

import argparse
import hashlib
import io
from pathlib import Path

import pypdfium2 as pdfium
from PIL import Image


SOURCE_SHA256 = {
    "file": "9911ca1154dfad8b18c1154641ba203b54294789ee08fea86a76589fc75adc15",
    "folder": "d007e10eff85cda678640fa944bd484197f6ef447bb2d2ece75b5b8856cc72c5",
}
THEMES = (
    ("north", (0.0, 0.0, 117.279, 95.3246), (242, 242, 242)),
    ("vanilla", (117.279, 0.0, 234.557, 95.3246), (241, 228, 187)),
    ("breeze", (234.557, 0.0, 360.49, 95.3246), (33, 56, 64)),
    ("twilight", (360.49, 0.0, 486.424, 95.3246), (51, 51, 51)),
)
FILE_INKS = (
    ((237, 237, 237),),
    ((234, 220, 183),),
    ((30, 52, 58),),
    ((48, 48, 48),),
)
FOLDER_INKS = (
    ((91, 91, 91), (178, 178, 178)),
    ((4, 63, 97), (93, 165, 181)),
    ((4, 63, 97), (93, 165, 181)),
    ((91, 91, 91), (178, 178, 178)),
)
OUTPUT_SIZE = (512, 384)
RENDER_SCALE = 8
FOLDER_BACKGROUND_BLEND = 0.20


def blended_inks(inks, background, amount):
    return tuple(
        tuple(round((1.0 - amount) * channel + amount * backdrop)
              for channel, backdrop in zip(ink, background))
        for ink in inks
    )


def dematte(image, background, source_inks, output_inks):
    result = Image.new("RGBA", image.size)
    source_pixels = image.convert("RGB").load()
    output_pixels = result.load()
    vectors = []
    for ink in source_inks:
        vector = tuple(ink[index] - background[index] for index in range(3))
        vectors.append((vector, sum(component * component for component in vector)))

    for y in range(image.height):
        for x in range(image.width):
            pixel = source_pixels[x, y]
            if pixel == background:
                continue
            best = None
            for index, (vector, denominator) in enumerate(vectors):
                delta = tuple(pixel[channel] - background[channel] for channel in range(3))
                alpha = max(0.0, min(1.0, sum(delta[channel] * vector[channel]
                                                for channel in range(3)) / denominator))
                reconstructed = tuple(background[channel] + alpha * vector[channel]
                                      for channel in range(3))
                error = sum((pixel[channel] - reconstructed[channel]) ** 2
                            for channel in range(3))
                if best is None or error < best[0]:
                    best = (error, alpha, index)
            _, alpha, index = best
            alpha_byte = round(alpha * 255)
            if alpha_byte:
                output_pixels[x, y] = (*output_inks[index], alpha_byte)
    return result


def render_panel(source_data, bounds, background, source_inks, output_inks):
    with pdfium.PdfDocument(source_data) as document:
        if len(document) != 1:
            raise ValueError("Expected approved single-page Illustrator artwork.")
        page = document[0]
        page.set_cropbox(*bounds)
        bitmap = page.render(scale=RENDER_SCALE, fill_color=(0, 0, 0, 0))
        try:
            rendered = bitmap.to_pil().convert("RGB")
        finally:
            bitmap.close()
    transparent = dematte(rendered, background, source_inks, output_inks)
    width = round(transparent.width * OUTPUT_SIZE[1] / transparent.height)
    scaled = transparent.resize((width, OUTPUT_SIZE[1]), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", OUTPUT_SIZE)
    canvas.alpha_composite(scaled, ((OUTPUT_SIZE[0] - width) // 2, 0))
    return canvas


def export_artwork(file_source, folder_source):
    sources = {"file": file_source.read_bytes(), "folder": folder_source.read_bytes()}
    for kind, data in sources.items():
        if hashlib.sha256(data).hexdigest() != SOURCE_SHA256[kind]:
            raise ValueError(f"{kind} source differs from the approved artwork.")

    result = {}
    for index, (theme, bounds, background) in enumerate(THEMES):
        file_image = render_panel(sources["file"], bounds, background, FILE_INKS[index],
                                  FILE_INKS[index])
        folder_output_inks = blended_inks(FOLDER_INKS[index], background,
                                           FOLDER_BACKGROUND_BLEND)
        folder_image = render_panel(sources["folder"], bounds, background, FOLDER_INKS[index],
                                    folder_output_inks)
        for kind, image in (("file", file_image), ("folder", folder_image)):
            buffer = io.BytesIO()
            image.save(buffer, format="PNG", optimize=True)
            result[f"src/ui/qt/artwork/empty-{kind}-{theme}.png"] = buffer.getvalue()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file-source", type=Path, required=True)
    parser.add_argument("--folder-source", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    for relative, data in export_artwork(args.file_source, args.folder_source).items():
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
