"""Extract the approved catalog-tool AI artwork into compact QPainter paths.

Build-time only: pypdf and fonttools; --probes additionally needs pypdfium2.
The masters are read-only inputs, never runtime assets. F5 is outlined from the
embedded TrueType data in memory, without installing or exporting a font.
"""

import argparse
from dataclasses import dataclass
import hashlib
import io
from pathlib import Path

from fontTools.pens.basePen import BasePen
from fontTools.ttLib import TTFont
from pypdf import PdfReader


MASTERS = {
    "ViewAll.ai": "f2adf66e5fc6a58a56e37e7c398ddfc9dd15a80276fbc5132318bc9906f85065",
    "f5.ai": "e0bb6fc9520c9f06051b24333c48df25e7118b876a2538803ba1fb5b29bc2ca1",
}


@dataclass
class Layer:
    ink: tuple
    commands: list


class OutlinePen(BasePen):
    def __init__(self, glyphs, origin, scale):
        super().__init__(glyphs)
        self.origin = origin
        self.scale = scale
        self.commands = []

    def emit(self, operation, *points):
        coordinates = tuple(self.origin[i] + value * self.scale
                            for point in points for i, value in enumerate(point))
        self.commands.append((operation, coordinates))

    def _moveTo(self, point):
        self.emit("moveTo", point)

    def _lineTo(self, point):
        self.emit("lineTo", point)

    def _curveToOne(self, first, second, end):
        self.emit("cubicTo", first, second, end)

    def _qCurveToOne(self, control, end):
        self.emit("quadTo", control, end)

    def _closePath(self):
        self.emit("closeSubpath")


def extract_layers(data):
    page = PdfReader(io.BytesIO(data)).pages[0]
    fonts = {}
    for name, reference in page["/Resources"].get("/Font", {}).items():
        font = reference.get_object()
        fonts[name] = (font, TTFont(io.BytesIO(font["/FontDescriptor"]["/FontFile2"].get_data())))
    ink, offset, font_name = (0, 0, 0), (0, 0), None
    stack, commands, layers = [], [], []
    for values, operation in page.get_contents().operations:
        if operation == b"q":
            stack.append((ink, offset, font_name))
        elif operation == b"Q":
            ink, offset, font_name = stack.pop()
        elif operation == b"cm":
            a, b, c, d, x, y = map(float, values)
            if (a, b, c, d) != (1, 0, 0, 1):
                raise ValueError("Expected translated paths only")
            offset = (offset[0] + x, offset[1] + y)
        elif operation == b"scn":
            if len(values) != 3:
                raise ValueError("Expected RGB inks")
            ink = tuple(round(float(value) * 255) for value in values)
        elif operation in (b"m", b"l", b"c"):
            name = {b"m": "moveTo", b"l": "lineTo", b"c": "cubicTo"}[operation]
            commands.append((name, tuple(float(v) + offset[i % 2]
                                         for i, v in enumerate(values))))
        elif operation == b"h":
            commands.append(("closeSubpath", ()))
        elif operation == b"re":
            # Only rectangular theme swatches and the page clip use this operator.
            commands = []
        elif operation == b"n":
            commands = []
        elif operation == b"f":
            if any(op == "cubicTo" for op, _ in commands):
                if commands[-1][0] != "closeSubpath":
                    commands.append(("closeSubpath", ()))
                layers.append(Layer(ink, commands))
            commands = []
        elif operation == b"Tf":
            font_name, size = values
            if size != 1:
                raise ValueError("Unexpected font size")
        elif operation == b"Tm":
            a, b, c, d, x, y = map(float, values)
            if (a, b, c, d) != (12, 0, 0, 12):
                raise ValueError("Unexpected text transform")
            text_origin = (x + offset[0], y + offset[1])
        elif operation == b"Tj":
            if str(values[0]) != "F5":
                raise ValueError("Only the authored F5 text may be outlined")
            font, tt = fonts[font_name]
            glyphs, cmap = tt.getGlyphSet(), tt.getBestCmap()
            x, y = text_origin
            outlines = []
            for character in "F5":
                pen = OutlinePen(glyphs, (x, y), 12 / tt["head"].unitsPerEm)
                glyphs[cmap[ord(character)]].draw(pen)
                outlines.extend(pen.commands)
                x += float(font["/Widths"][ord(character) - font["/FirstChar"]]) * 12 / 1000
            layers.append(Layer(ink, outlines))
        elif operation not in (b"BDC", b"EMC", b"cs", b"ri", b"gs", b"W", b"BT", b"ET"):
            raise ValueError(f"Unexpected PDF operation: {operation!r}")
    if stack or commands:
        raise ValueError("Unbalanced artwork stream")
    return layers


def normalize(layer, center):
    return [(name, tuple(round(value - center[i % 2], 4) for i, value in enumerate(values)))
            for name, values in layer.commands]


def verify_geometry(expected_paths, actual_paths):
    for expected, actual in zip(expected_paths, actual_paths, strict=True):
        if len(expected) != len(actual):
            raise ValueError("Artwork state changed its path topology")
        for (op, values), (other_op, other_values) in zip(expected, actual, strict=True):
            if op != other_op or len(values) != len(other_values) or any(
                    abs(a - b) > 0.0011 for a, b in zip(values, other_values, strict=True)):
                raise ValueError("Artwork state changed geometry beyond AI rounding")


def variants(data, eye):
    layers = extract_layers(data)
    stride = 5 if eye else 3
    if len(layers) != stride * 8:
        raise ValueError("Expected four passive/hover pairs")
    result = []
    for i in range(0, len(layers), stride):
        group = layers[i:i + stride]
        if eye:
            x, y = group[0].commands[0][1]
            center = (x, y - 10.7085)
        else:
            x, y = group[1].commands[0][1]
            center = (x - 10.7085, y)
            group = [Layer(group[1].ink, group[1].commands + group[2].commands), group[0]]
        result.append((center, [normalize(layer, center) for layer in group],
                       [layer.ink for layer in group]))
    result.sort(key=lambda item: item[0][0])
    for i, (_, paths, _) in enumerate(result):
        verify_geometry(result[0 if eye else i % 2][1], paths)
        if not eye:
            verify_geometry(result[0][1][:1], paths[:1])
    if not eye:
        # F5 uses authored Thin/Regular outlines for passive/hover. Keep the
        # painter's shared-path API by making the unused text layer transparent.
        paths = [result[0][1][0], result[0][1][1], result[1][1][1]]
        transparent = (0, 0, 0, 0)
        result = [(center, paths, [inks[0], inks[1] if i % 2 == 0 else transparent,
                                  inks[1] if i % 2 else transparent])
                  for i, (center, _, inks) in enumerate(result)]
    return result


def number(value):
    return f"{value:.4f}".rstrip("0").rstrip(".") if value else "0"


def header(artwork):
    lines = ["#pragma once", "", "// Generated by scripts/generate_catalog_tool_artwork.py.",
             "// Authored points, centered on the ring; PDF Y points upward."]
    lines.extend(f"// {name}: SHA-256 {digest}" for name, digest in MASTERS.items())
    lines.extend(["", "#include <QColor>", "#include <QPainterPath>", "", "#include <array>",
                  "", "namespace vove::ui::artwork {", ""])
    for name, states in artwork.items():
        paths = states[0][1]
        # QColor(QRgb) in the button intentionally treats RGB inks as opaque.
        # Preserve transparent outline selectors as QColor objects instead.
        has_alpha = any(len(ink) == 4 for _, _, inks in states for ink in inks)
        color_type = "QColor" if has_alpha else "QRgb"
        qualifier = "const" if has_alpha else "constexpr"
        lines.extend([f"inline const std::array<QPainterPath, {len(paths)}> &{name}_paths() {{",
                      "    static const auto paths = [] {",
                      f"        std::array<QPainterPath, {len(paths)}> result;"])
        for i, path in enumerate(paths):
            lines.append(f"        result[{i}].setFillRule(Qt::WindingFill);")
            for operation, values in path:
                lines.append(f"        result[{i}].{operation}({', '.join(map(number, values))});")
        lines.extend(["        return result;", "    }();", "    return paths;", "}", "",
                      "// white, vanilla, blue, dark_gray; passive then hover in each pair.",
                      f"inline {qualifier} std::array<std::array<{color_type}, {len(paths)}>, 8> {name}_inks{{{{"])
        for _, _, inks in states:
            colors = [f"{'QColor' if has_alpha else 'qRgb'}({', '.join(map(str, ink))})"
                      for ink in inks]
            lines.append("    {{" + ", ".join(colors) + "}},")
        lines.extend(["}};", ""])
    lines.extend(["} // namespace vove::ui::artwork", ""])
    return "\n".join(lines).encode("ascii")


def probes(directory, data, states, name):
    import pypdfium2 as pdfium

    directory.mkdir(parents=True, exist_ok=True)
    with pdfium.PdfDocument(data) as document:
        bitmap = document[0].render(scale=3)
        bitmap.to_pil().save(directory / f"{name}-master.png")
        bitmap.close()
    for i, (center, _, _) in enumerate(states):
        for dpr in (1, 1.25, 1.5, 2, 4):
            with pdfium.PdfDocument(data) as document:
                # Binary-exact crop edges avoid PDFium rounding 24.000002 up to 25 pixels.
                x, y = (round(value * 4096) / 4096 for value in center)
                page = document[0]
                page.set_cropbox(x - 12, y - 12, x + 12, y + 12)
                bitmap = page.render(scale=dpr)
                bitmap.to_pil().save(directory / f"{name}-{i}-{dpr:g}x.png")
                bitmap.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--probes", type=Path, help="Optional visual references under WORK/out")
    args = parser.parse_args()
    if args.probes and not args.probes.resolve().is_relative_to((args.root / "out").resolve()):
        parser.error("Visual probes must stay under WORK/out")
    artwork = {}
    for filename, digest in MASTERS.items():
        data = (args.root / "design" / filename).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"Master differs from the approved artwork: {filename}")
        name = "catalog_eye" if filename == "ViewAll.ai" else "catalog_refresh"
        artwork[name] = variants(data, filename == "ViewAll.ai")
        if args.probes:
            probes(args.probes, data, artwork[name], name)
    output = args.root / "src/ui/qt/catalog_tool_artwork.hpp"
    generated = header(artwork)
    if args.check:
        if output.read_bytes().replace(b"\r\n", b"\n") != generated:
            raise ValueError("Generated catalog artwork is out of date")
    else:
        output.write_bytes(generated)
    print(f"{output}: {len(generated)} bytes; eight verified variants per tool")


if __name__ == "__main__":
    main()
