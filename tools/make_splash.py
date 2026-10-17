#!/usr/bin/env python3
"""Draw the "Made with Pocket2D" splash image from the engine icon and font.

  python3 tools/make_splash.py

Writes .engine/system/splash.png (transparent, light text for a dark
background). The engine build compiles that file into the runtime, so a
shipped game shows the splash without the .engine folder.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SYSTEM = ROOT / ".engine/system"
WIDTH, HEIGHT = 760, 260
ICON = 220


def main():
    icon = Image.open(SYSTEM / "icon.png").convert("RGBA")
    icon = icon.crop(icon.getbbox())
    scale = ICON / max(icon.size)
    icon = icon.resize((round(icon.width * scale), round(icon.height * scale)), Image.LANCZOS)

    out = Image.new("RGBA", (WIDTH, HEIGHT))
    out.alpha_composite(icon, (0, (HEIGHT - icon.height) // 2))
    draw = ImageDraw.Draw(out)
    font = SYSTEM / "fonts/system.ttf"
    small = ImageFont.truetype(str(font), 34)
    large = ImageFont.truetype(str(font), 96)
    x = ICON + 36
    draw.text((x + 4, 52), "Made with", font=small, fill=(154, 163, 181, 255))
    draw.text((x, 92), "Pocket2D", font=large, fill=(242, 244, 248, 255), stroke_width=1,
              stroke_fill=(242, 244, 248, 255))
    out = out.crop(out.getbbox())
    out.save(SYSTEM / "splash.png", optimize=True)
    print("wrote", (SYSTEM / "splash.png").relative_to(ROOT), out.size)


if __name__ == "__main__":
    main()
