"""Render editable performance-group SVGs using ImageMagick and the TGA skill.

Run from any directory: python Jamma/resources/textures/source/performance-groups/render.py
Outputs stay in workspace .staging/performance-groups until --install is supplied.
Requires Python 3, ImageMagick and PowerShell; no Python packages are needed.
"""

import argparse
import hashlib
import pathlib
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET


SOURCE = pathlib.Path(__file__).resolve().parent
ROOT = SOURCE.parents[4]
STAGING = ROOT / ".staging" / "performance-groups"
TEXTURES = SOURCE.parents[1]
NS = "{http://www.w3.org/2000/svg}"


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True, capture_output=True)


def validate(path, width, height, border, filled):
    data = path.read_bytes()
    header = struct.unpack("<BBBHHBHHHHBB", data[:18])
    assert header[2] == 2 and header[1] == 0, f"{path}: uncompressed truecolor required"
    assert header[8:10] == (width, height), f"{path}: dimensions"
    assert header[10] == 32 and header[11] == 8, f"{path}: bottom-left + 8 alpha bits"
    assert border * 2 < width and border * 2 < height
    pixels = data[18 + header[0]:18 + header[0] + width * height * 4]
    assert len(pixels) == width * height * 4

    def pixel(x, y):
        # TGA origin is bottom left, authored coordinates are top left.
        offset = ((height - y - 1) * width + x) * 4
        return tuple(pixels[offset:offset + 4])

    assert all(pixels[i] == pixels[i + 1] == pixels[i + 2]
               for i in range(0, len(pixels), 4)), f"{path}: tintable grayscale"
    assert pixel(0, 0)[3] == 0, f"{path}: transparent corner"
    assert pixel(width // 2, 0)[3] == 255, f"{path}: sharp straight border"
    assert pixel(width // 2, height // 2)[3] == (255 if filled else 0)
    assert any(0 < pixels[i] < 255 for i in range(3, len(pixels), 4)), f"{path}: curved AA"
    assert any(pixels[i - 1] > pixels[i] for i in range(3, len(pixels), 4)
               if 0 < pixels[i] < 255), f"{path}: RGB must retain straight alpha"
    # Exact ninepatch boundary: every pixel on either side of the stretchable
    # centre must match the centre's corresponding row/column. This catches
    # slicing through a corner or into a nonuniform edge transition.
    for y in range(height):
        assert pixel(border, y) == pixel(width // 2, y) == pixel(width - border - 1, y)
    for x in range(width):
        assert pixel(x, border) == pixel(x, height // 2) == pixel(x, height - border - 1)
    print(f"Validated {path.name}: {width}x{height}, type 2 BGRA, bottom-left, slices {border}/{border}")


def render(source):
    svg = ET.parse(source).getroot()
    width, height = int(svg.attrib["width"]), int(svg.attrib["height"])
    mask = svg.find(f"{NS}defs/{NS}mask")
    assert mask is not None
    border = int(svg.attrib["data-slice"])
    filled = svg.attrib["data-filled"] == "true"
    mask_svg = ET.Element(f"{NS}svg", width=str(width), height=str(height),
                          viewBox=f"0 0 {width} {height}")
    for child in mask:
        mask_svg.append(child)
    rgb = STAGING / f"{source.stem}_rgb.svg"
    alpha = STAGING / f"{source.stem}_alpha.svg"
    ET.ElementTree(mask_svg).write(alpha)
    svg.remove(svg.find(f"{NS}defs"))
    svg.find(f"{NS}g").attrib.pop("mask")
    ET.ElementTree(svg).write(rgb)
    rgb_png = rgb.with_suffix(".png")
    alpha_png = alpha.with_suffix(".png")
    png = STAGING / f"{source.stem}_8x.png"
    run("magick", "-background", "black", "-density", "768", rgb, rgb_png)
    run("magick", "-background", "black", "-density", "768", alpha, alpha_png)
    # RGB canvas is opaque and complete before the sole silhouette/hole mask.
    run("magick", rgb_png, alpha_png, "-alpha", "off", "-compose", "CopyOpacity", "-composite", png)
    target = STAGING / f"{source.stem}.tga"
    run("powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
        ROOT / ".agents/skills/tga-icon-gen/render-tga-icon.ps1",
        "-InputPng", png, "-OutputTga", target, "-TargetSize", f"{width}x{height}")
    # ImageMagick 7 carries the PNG's top-left orientation into TGA. The
    # engine contract requires bottom-left, so explicitly set the export.
    normalized = STAGING / f"{source.stem}_bottom_left.tga"
    run("magick", target, "-orient", "BottomLeft", "-compress", "none", normalized)
    normalized.replace(target)
    validate(target, width, height, border, filled)
    return target


def contact_sheet():
    def tint_args(color):
        args = []
        for channel, offset in zip("RGB", [1, 3, 5]):
            args.extend(["-channel", channel, "-evaluate", "multiply", str(int(color[offset:offset + 2], 16) / 255)])
        return args + ["+channel"]

    cells = []
    families = ["performance_group", "performance_group_on", "performance_group_add"]
    colors = [("Mute", "#4296ff"), ("Selection", "#ffdf35"), ("Neutral +", "#ffffff")]
    for row, (family, (label, tint)) in enumerate(zip(families, colors)):
        for column, suffix in enumerate(["", "_over", "_down"]):
            cell = STAGING / f"cell_{row}_{column}.png"
            # Preview the intended 32px assets at 4x and compare both group tints.
            run("magick", STAGING / f"{family}{suffix}.tga", *tint_args(tint), "-filter", "point",
                "-resize", "128x128", "-background", "#20242a", "-alpha", "remove",
                "-alpha", "off", "-gravity", "center", "-fill", "white", "-font", "Arial",
                "-pointsize", "44", "-annotate", "+0+0", "+" if row == 2 else "1",
                "-gravity", "south", "-background", "#20242a", "-extent", "180x170",
                "-pointsize", "12", "-annotate", "+0+8", f"{label} {suffix or 'base'}", cell)
            cells.append(cell)
    # Also show filled states with the blue tint and empty states with yellow.
    for row, (family, tint) in enumerate([(families[1], colors[0][1]), (families[0], colors[1][1])], 3):
        for column, suffix in enumerate(["", "_over", "_down"]):
            cell = STAGING / f"cell_{row}_{column}.png"
            run("magick", STAGING / f"{family}{suffix}.tga", *tint_args(tint), "-filter", "point",
                "-resize", "128x128", "-background", "#20242a", "-alpha", "remove", "-alpha", "off",
                "-gravity", "center", "-fill", "white", "-font", "Arial", "-pointsize", "44",
                "-annotate", "+0+0", "1", "-gravity", "south", "-background", "#20242a",
                "-extent", "180x170", "-pointsize", "12", "-annotate", "+0+8",
                f"{'Mute filled' if row == 3 else 'Selection empty'} {suffix or 'base'}", cell)
            cells.append(cell)
    for column, (label, tint) in enumerate(colors):
        cell = STAGING / f"cell_holder_{column}.png"
        run("magick", STAGING / "performance_group_holder.tga", *tint_args(tint),
            "-filter", "point", "-resize", "160x160", "-background", "#20242a",
            "-alpha", "remove", "-alpha", "off", "-gravity", "center", "-fill", "white",
            "-font", "Arial", "-pointsize", "18", "-annotate", "+0+0", label,
            "-background", "#20242a", "-extent", "180x170", cell)
        cells.append(cell)
    run("magick", "montage", *cells, "-tile", "3x6", "-geometry", "+0+0",
        STAGING / "contact-sheet.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    args = parser.parse_args()
    STAGING.mkdir(parents=True, exist_ok=True)
    outputs = [render(source) for source in sorted(SOURCE.glob("*.svg"))]
    for family in ["performance_group", "performance_group_on", "performance_group_add"]:
        alpha_planes = []
        for suffix in ["", "_over", "_down"]:
            data = (STAGING / f"{family}{suffix}.tga").read_bytes()
            alpha_planes.append(data[21:18 + 32 * 32 * 4:4])
        assert alpha_planes[0] == alpha_planes[1] == alpha_planes[2], f"{family}: aligned state silhouettes"
    contact_sheet()
    if args.install:
        for staged in outputs:
            destination = TEXTURES / staged.name
            if destination.exists():
                index = 1
                backup = destination.with_stem(destination.stem + "_previous")
                while backup.exists():
                    index += 1
                    backup = destination.with_stem(destination.stem + f"_previous_{index}")
                destination.rename(backup)
            shutil.copyfile(staged, destination)
            assert hashlib.sha256(staged.read_bytes()).digest() == hashlib.sha256(destination.read_bytes()).digest()
    print(f"Contact sheet: {STAGING / 'contact-sheet.png'}")


if __name__ == "__main__":
    main()
