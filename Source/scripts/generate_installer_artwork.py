"""Generate font-independent Windows installer artwork from the approved Install.ai layout.

The runtime installer never loads LINE Seed JP. Static interface copy and button captions are
rasterized during development from the pinned font files; the license remains a native text
control. The resulting PNGs are the only artwork embedded in Setup.
"""

import argparse
import hashlib
import io
import json
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


PINNED_SHA256 = {
    "design/Install.ai": "c73ded64ae3818ea59dc407fe993ea06ec1c84e8758d78cc289e949a12a63172",
    "design/Font/LINESeedJP-Regular.ttf":
        "167611a1f3dd3bc5b6a0beb47c20d15d930f63abdc0e5f035adbe1b4b6ef360d",
    "design/Font/LINESeedJP-Bold.ttf":
        "d7baef3d9d1b5d3144063128890f1f80c7d3c88225697d6a4bce4fe55957e31e",
    "src/ui/qt/artwork/logo-vanilla.png":
        "c24ca884bb14b0fcaa74044e6d925a644032ccbf80ded477720ceb6ec391b771",
}

SCALE = 2
SIZE = (540, 860)
BACKGROUND = "#F1E4BB"
PANEL = "#FFF9EA"
INK = "#213840"
MUTED = "#6F6F6F"
CYAN = "#5EA5B6"
ORANGE = "#FF7900"
RED = "#FF7474"
GREEN = "#5DBB72"
PALE = "#E8DDBB"


COPY = {
    "ru": {
        "main": "Основной пакет",
        "tagline": "VO-VE - ультралёгкий просмотрщик графики",
        "desktop": "создать значок на Рабочем столе",
        "start": "создать пункт меню в панели Пуск",
        "extras": "Дополнения",
        "required": "Обязательные",
        "required_note": "Без этих дополнений работоспособность VO-VE будет ограничена",
        "ghost": "Ghostscript",
        "ghost_note": "Требуется для отображения PS, EPS и классических PostScript AI",
        "recommended": "Горячо рекомендуемые:",
        "recommended_note":
            "С этими дополнениями вы сможете насладиться работой VO-VE в полной мере",
        "font": "шрифт LINE Seed JP",
        "font_note":
            "Обеспечивает отличный внешний вид интерфейса (как в этом окне), без него выглядит хуже",
        "everything": "Everything",
        "everything_note":
            "Обеспечивает мгновенный глобальный поиск макетов в любых уголках системного диска "
            "и на сетевых локальных дисках",
        "note_prefix": "N.B.",
        "note": "Скачанные дополнения устанавливаются отдельно от VO-VE",
        "license": "Лицензионное соглашение",
        "accept": "Принимаю",
        "decline": "Не принимаю",
        "download": "Скачать",
        "install": "Установить",
        "present": "есть",
        "missing": "отсутствует",
    },
    "en": {
        "main": "Main package",
        "tagline": "VO-VE - an ultra-lightweight graphics viewer",
        "desktop": "create a Desktop shortcut",
        "start": "create a Start menu shortcut",
        "extras": "Additions",
        "required": "Required",
        "required_note": "Without these additions some VO-VE formats will be unavailable",
        "ghost": "Ghostscript",
        "ghost_note": "Required for PS, EPS and classic PostScript AI files",
        "recommended": "Highly recommended:",
        "recommended_note": "These additions provide the complete VO-VE experience",
        "font": "LINE Seed JP font",
        "font_note": "Provides the intended interface appearance (as in this window)",
        "everything": "Everything",
        "everything_note":
            "Provides instant global search across local system and indexed network drives",
        "note_prefix": "N.B.",
        "note": "Downloaded additions must be installed separately from VO-VE",
        "license": "License agreement",
        "accept": "I accept",
        "decline": "I do not accept",
        "download": "Download",
        "install": "Install",
        "present": "detected",
        "missing": "not found",
    },
}


def scaled(value):
    return round(value * SCALE)


def verify_inputs(root):
    for relative, expected in PINNED_SHA256.items():
        path = root / relative
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"Approved installer input changed: {relative}")


def font(root, size, bold=False):
    name = "LINESeedJP-Bold.ttf" if bold else "LINESeedJP-Regular.ttf"
    return ImageFont.truetype(root / "design" / "Font" / name, scaled(size))


def text(draw, root, position, value, size, fill=INK, bold=False):
    draw.text(tuple(scaled(item) for item in position), value, font=font(root, size, bold),
              fill=fill, spacing=scaled(2))


def wrapped(draw, root, position, value, width, size, fill=MUTED, bold=False, line_gap=2):
    selected = []
    line = ""
    selected_width = scaled(width)
    selected_font = font(root, size, bold)
    for word in value.split():
        candidate = word if not line else f"{line} {word}"
        if line and draw.textlength(candidate, font=selected_font) > selected_width:
            selected.append(line)
            line = word
        else:
            line = candidate
    if line:
        selected.append(line)
    x, y = position
    for index, line in enumerate(selected):
        text(draw, root, (x, y + index * (size + line_gap)), line, size, fill, bold)


def background(root, language, version):
    copy = COPY[language]
    image = Image.new("RGB", tuple(scaled(item) for item in SIZE), BACKGROUND)
    draw = ImageDraw.Draw(image)

    logo = Image.open(root / "src" / "ui" / "qt" / "artwork" / "logo-vanilla.png").convert(
        "RGBA")
    logo = logo.resize((scaled(416), scaled(80)), Image.Resampling.LANCZOS)
    image.paste(logo, (scaled(62), scaled(31)), logo)

    text(draw, root, (36, 14), version, 15, CYAN)
    text(draw, root, (455, 14), "RU", 14, ORANGE if language == "ru" else INK)
    text(draw, root, (479, 14), "|", 14, INK)
    text(draw, root, (491, 14), "EN", 14, ORANGE if language == "en" else INK)

    draw.line((scaled(36), scaled(139), scaled(504), scaled(139)), fill=CYAN, width=scaled(1))
    text(draw, root, (36, 146), copy["main"], 22, INK, True)
    text(draw, root, (36, 173), copy["tagline"], 14, INK)
    text(draw, root, (68, 207), copy["desktop"], 14, INK)
    text(draw, root, (68, 242), copy["start"], 14, INK)

    draw.line((scaled(36), scaled(279), scaled(504), scaled(279)), fill=CYAN, width=scaled(1))
    text(draw, root, (36, 286), copy["extras"], 22, INK, True)
    text(draw, root, (36, 313), copy["required"], 15, ORANGE)
    text(draw, root, (36, 335), copy["required_note"], 11, MUTED)
    text(draw, root, (68, 362), copy["ghost"], 14, INK)
    text(draw, root, (68, 386), copy["ghost_note"], 11, MUTED)

    text(draw, root, (36, 418), copy["recommended"], 15, CYAN)
    text(draw, root, (36, 440), copy["recommended_note"], 11, MUTED)
    text(draw, root, (68, 468), copy["font"], 14, INK)
    wrapped(draw, root, (68, 492), copy["font_note"], 410, 11)
    text(draw, root, (68, 528), copy["everything"], 14, INK)
    wrapped(draw, root, (68, 552), copy["everything_note"], 330, 11)

    text(draw, root, (82, 624), copy["note_prefix"], 11, ORANGE)
    wrapped(draw, root, (111, 624), copy["note"], 365, 10, MUTED, line_gap=1)
    draw.line((scaled(36), scaled(650), scaled(504), scaled(650)), fill=CYAN, width=scaled(1))
    text(draw, root, (36, 657), copy["license"], 21, INK, True)

    draw.rectangle((scaled(36), scaled(681), scaled(504), scaled(770)), fill=PANEL,
                   outline="#D8C47F", width=scaled(1))
    text(draw, root, (68, 786), copy["accept"], 13, GREEN)
    text(draw, root, (68, 818), copy["decline"], 13, RED)
    return image


def circle(selected):
    image = Image.new("RGBA", (scaled(32), scaled(32)))
    draw = ImageDraw.Draw(image)
    color = CYAN if selected else PALE
    draw.ellipse((scaled(2), scaled(2), scaled(30), scaled(30)), fill=color)
    draw.ellipse((scaled(10), scaled(10), scaled(22), scaled(22)), fill=BACKGROUND)
    return image


def button(root, caption, enabled=True):
    image = Image.new("RGBA", (scaled(92), scaled(30)))
    draw = ImageDraw.Draw(image)
    fill = CYAN if enabled else "#B8B19A"
    outline = INK if enabled else "#888273"
    draw.rounded_rectangle(
        (0, scaled(2), image.width - 1, image.height - 1),
        radius=scaled(10), fill=outline)
    draw.rounded_rectangle(
        (scaled(1), 0, image.width - scaled(1) - 1, image.height - scaled(3)),
        radius=scaled(9), fill=fill, outline=outline, width=scaled(1))
    selected_font = font(root, 13)
    box = draw.textbbox((0, 0), caption, font=selected_font)
    width = box[2] - box[0]
    height = box[3] - box[1]
    draw.text(((image.width - width) // 2, (image.height - height) // 2 - box[1] - scaled(1)), caption,
              font=selected_font, fill="#FFFFFF" if enabled else BACKGROUND)
    return image


def status(root, caption, color):
    image = Image.new("RGBA", (scaled(110), scaled(22)))
    draw = ImageDraw.Draw(image)
    draw.text((0, 0), caption, font=font(root, 12), fill=color)
    return image


def encode(image):
    output = io.BytesIO()
    image.save(output, "PNG", optimize=True)
    return output.getvalue()


def project_version(root):
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(VO_VE VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES", cmake)
    if not match:
        raise ValueError("Could not read the VO-VE version from CMakeLists.txt")
    return match.group(1)


def generate(root, version):
    verify_inputs(root)
    enabled_download = button(root, COPY["en"]["download"])
    disabled_download = button(root, COPY["en"]["download"], False)
    enabled_install = button(root, COPY["en"]["install"])
    disabled_install = button(root, COPY["en"]["install"], False)
    for name, enabled, disabled in (
        ("download", enabled_download, disabled_download),
        ("install", enabled_install, disabled_install),
        ("toggle", circle(True), circle(False)),
    ):
        if enabled.size != disabled.size or enabled.getbbox() != disabled.getbbox():
            raise ValueError(f"Enabled and disabled {name} artwork changed geometry")
    result = {
        "installer-background-ru.png": encode(background(root, "ru", version)),
        "installer-background-en.png": encode(background(root, "en", version)),
        "installer-toggle-on.png": encode(circle(True)),
        "installer-toggle-off.png": encode(circle(False)),
    }
    for language, copy in COPY.items():
        result[f"installer-status-present-{language}.png"] = encode(
            status(root, copy["present"], GREEN))
        result[f"installer-status-missing-{language}.png"] = encode(
            status(root, copy["missing"], RED))
        result[f"installer-download-{language}.png"] = encode(button(root, copy["download"]))
        result[f"installer-download-disabled-{language}.png"] = encode(
            button(root, copy["download"], False))
        result[f"installer-install-{language}.png"] = encode(button(root, copy["install"]))
        result[f"installer-install-disabled-{language}.png"] = encode(
            button(root, copy["install"], False))
    manifest = {
        "schema": 1,
        "version": version,
        "generator": {
            "path": "scripts/generate_installer_artwork.py",
            "sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        },
        "inputs": [
            {"path": path, "sha256": digest}
            for path, digest in sorted(PINNED_SHA256.items())
        ],
        "files": [
            {
                "path": name,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            for name, data in sorted(result.items())
        ],
    }
    result["installer-artwork-manifest.json"] = (
        json.dumps(manifest, ensure_ascii=True, indent=2) + "\n"
    ).encode("ascii")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--version", help="Override the version read from CMakeLists.txt")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    version = args.version or project_version(args.root)
    destination = args.root / "packaging" / "windows" / "artwork"
    for name, data in generate(args.root, version).items():
        path = destination / name
        if args.check:
            if not path.is_file() or path.read_bytes() != data:
                raise ValueError(f"Generated installer artwork is out of date: {name}")
        else:
            destination.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        print(f"{name}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
