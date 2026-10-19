"""Build a small flash-resident animation set; requires Pillow and imageio-ffmpeg."""
import hashlib
import json
import zlib
from pathlib import Path
from urllib.parse import quote
from urllib.request import urlopen

import imageio_ffmpeg
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
REVISION = "ddc63d9d8aa0701282e68cc6f3fdc26d5c81e793"
BASE = f"https://raw.githubusercontent.com/Bai-Cute/big-blue-fish-desktop-pet/{REVISION}/"
CLIPS = [("idle", "待机呼吸休闲"), ("run", "原地左转奔跑"),
         ("wave", "点击回应-元气挥手"), ("bubble", "鲸鱼吐泡泡特效")]
SIZE, FPS = 168, 6
BACKGROUND = (8, 20, 41, 255)


def array(name, values, kind="uint8_t"):
    rows = [", ".join(str(value) for value in values[n:n + 24]) for n in range(0, len(values), 24)]
    return f"const {kind} {name}[] PROGMEM = {{\n" + ",\n".join(rows) + "\n};\n"


def source_frames(path):
    reader = imageio_ffmpeg.read_frames(str(path), pix_fmt="rgba", bits_per_pixel=32,
                                       input_params=["-c:v", "libvpx-vp9"],
                                       output_params=["-vf", f"fps={FPS}"])
    metadata = next(reader)
    count = 0
    for raw in reader:
        if len(raw) != metadata["size"][0] * metadata["size"][1] * 4:
            raise ValueError(f"Invalid RGBA frame: {path.name}")
        count += 1
        yield Image.frombytes("RGBA", metadata["size"], raw)
    if abs(count - metadata["duration"] * FPS) > 2:
        raise ValueError(f"Animation duration mismatch: {path.name}")


def main():
    cache = ROOT / ".cache/pet"
    cache.mkdir(parents=True, exist_ok=True)
    bounds = []
    for name, original in CLIPS:
        path = cache / f"{name}.webm"
        remote = f"code/BigBlueFish.Windows/assets/fish/{original}.webm"
        if not path.exists():
            with urlopen(BASE + quote(remote), timeout=60) as response:
                path.write_bytes(response.read())
        for source in source_frames(path):
            box = source.getchannel("A").point(lambda alpha: 255 if alpha > 24 else 0).getbbox()
            if box:
                bounds.append(box)
    left, top = min(box[0] for box in bounds), min(box[1] for box in bounds)
    right, bottom = max(box[2] for box in bounds), max(box[3] for box in bounds)
    crop_size = max(right - left, bottom - top) + 8
    crop_x, crop_y = (left + right - crop_size) // 2, (top + bottom - crop_size) // 2
    crop = (crop_x, crop_y, crop_x + crop_size, crop_y + crop_size)
    print("Native source crop:", crop, "output:", SIZE, flush=True)
    output = ["#pragma once\n#include <Arduino.h>\n",
              f"constexpr int petSize = {SIZE};\nconstexpr int petFrameMs = {1000 // FPS};\n",
              "struct PetClip { const uint8_t *data; const uint32_t *offsets; const uint16_t *palette; uint16_t frames; };\n"]
    clips, manifest = [], []
    preview = []
    for name, original in CLIPS:
        path = cache / f"{name}.webm"
        remote = f"code/BigBlueFish.Windows/assets/fish/{original}.webm"
        frames = []
        for source in source_frames(path):
            source = source.crop(crop).resize((SIZE, SIZE), Image.Resampling.LANCZOS)
            frames.append(Image.alpha_composite(Image.new("RGBA", source.size, BACKGROUND), source).convert("RGB"))
        if not frames:
            raise ValueError(f"No decoded frames: {name}")
        sheet = Image.new("RGB", (SIZE * len(frames), SIZE))
        for index, frame in enumerate(frames):
            sheet.paste(frame, (index * SIZE, 0))
        palette = sheet.quantize(colors=128, method=Image.Quantize.MEDIANCUT)
        colors = palette.getpalette()[:384]
        rgb565 = [((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)
                  for r, g, b in zip(colors[::3], colors[1::3], colors[2::3])]
        data, offsets = bytearray(), [0]
        for frame in frames:
            indexed = frame.quantize(palette=palette, dither=Image.Dither.NONE)
            pixels = indexed.tobytes()
            compressed = zlib.compress(pixels, level=9)
            if zlib.decompress(compressed) != pixels or max(pixels) >= len(rgb565):
                raise ValueError(f"Invalid compressed palette frame: {name}")
            data.extend(compressed)
            offsets.append(len(data))
        output.extend([array(name + "Data", data), array(name + "Offsets", offsets, "uint32_t"),
                       array(name + "Palette", rgb565, "uint16_t")])
        clips.append(f"{{{name}Data, {name}Offsets, {name}Palette, {len(frames)}}}")
        manifest.append({"clip": name, "source": remote, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                         "frames": len(frames), "packed_bytes": len(data)})
        preview.append(frames[len(frames) // 2])
        print(name, len(frames), "frames,", len(data), "bytes", flush=True)
    output.append("const PetClip petClips[] = {" + ",".join(clips) + "};\n")
    (ROOT / "include/pet_frames.h").write_text("\n".join(output), encoding="ascii")
    (ROOT / "assets/pet-sources.json").write_text(json.dumps({"revision": REVISION, "size": SIZE, "fps": FPS, "compression": "zlib", "source_crop": crop, "clips": manifest}, indent=2) + "\n", encoding="utf-8")
    contact = Image.new("RGB", (SIZE * len(preview), SIZE))
    for index, frame in enumerate(preview):
        contact.paste(frame, (index * SIZE, 0))
    contact.resize((SIZE * len(preview) * 3, SIZE * 3)).save(cache / "preview.png")
    enlarged = Image.new("RGB", (240 * len(preview), 240), BACKGROUND[:3])
    for index, frame in enumerate(preview):
        enlarged.paste(frame, (index * 240 + 36, 36))
    enlarged.save(cache / "preview-large.png")
    with urlopen(BASE + "code/BigBlueFish.Windows/assets/fish/LICENSE.txt", timeout=30) as response:
        (ROOT / "assets/LICENSE-PC2005-cloud").write_bytes(response.read())


if __name__ == "__main__":
    main()
