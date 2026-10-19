from pathlib import Path

from PIL import Image


root = Path(__file__).resolve().parents[1]
source = root / "assets" / "whale-character-01.png"
target = root / "include" / "whale_bitmap.h"
size = 168
background = (9, 21, 43)

image = Image.open(source).convert("RGBA")
image.thumbnail((size, size), Image.Resampling.LANCZOS)
canvas = Image.new("RGBA", (size, size), background + (255,))
canvas.alpha_composite(image, ((size - image.width) // 2, (size - image.height) // 2))
rgb565 = []
pixels = canvas.convert("RGB").tobytes()
for index in range(0, len(pixels), 3):
    red, green, blue = pixels[index:index + 3]
    rgb565.append(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3))

rows = []
for start in range(0, len(rgb565), 12):
    rows.append("  " + ", ".join(f"0x{pixel:04X}" for pixel in rgb565[start:start + 12]))

target.write_text(
    "#pragma once\n\n"
    f"constexpr uint16_t whaleWidth = {size};\n"
    f"constexpr uint16_t whaleHeight = {size};\n"
    "const uint16_t whaleBitmap[whaleWidth * whaleHeight] PROGMEM = {\n"
    + ",\n".join(rows)
    + "\n};\n",
    encoding="ascii",
)
print(f"Wrote {target} ({target.stat().st_size} bytes)")
