"""Prepare the Orange Pi library; generated media stays outside Git."""
import concurrent.futures
import hashlib
import json
import struct
import zlib
from pathlib import Path
from urllib.parse import quote
from urllib.request import urlopen

from PIL import Image
from pet_media import BASE, BACKGROUND, CLIPS, FPS, REVISION, ROOT, SIZE, source_frames

DESTINATION = ROOT / ".cache/pet-library"


def prepare(entry):
    remote = entry["path"]
    name = Path(remote).stem
    identifier = hashlib.sha256(remote.encode()).hexdigest()[:16]
    source = DESTINATION / f"{identifier}.webm"
    packed = DESTINATION / f"{identifier}.pak"
    metadata = DESTINATION / f"{identifier}.json"
    if metadata.exists() and packed.exists():
        return json.loads(metadata.read_text(encoding="utf-8"))
    if not source.exists():
        with urlopen(BASE + quote(remote), timeout=60) as response:
            source.write_bytes(response.read())
    bounds = [frame.getchannel("A").point(lambda a: 255 if a > 24 else 0).getbbox()
              for frame in source_frames(source)]
    bounds = [box for box in bounds if box]
    if not bounds:
        raise ValueError(f"No visible frames: {name}")
    left, top = min(111, min(b[0] - 4 for b in bounds)), min(6, min(b[1] - 4 for b in bounds))
    right, bottom = max(470, max(b[2] + 4 for b in bounds)), max(365, max(b[3] + 4 for b in bounds))
    side = max(right - left, bottom - top)
    x, y = (left + right - side) // 2, (top + bottom - side) // 2
    frames = []
    for frame in source_frames(source):
        frame = frame.crop((x, y, x + side, y + side)).resize((SIZE, SIZE), Image.Resampling.LANCZOS)
        frames.append(Image.alpha_composite(Image.new("RGBA", frame.size, BACKGROUND), frame).convert("RGB"))
    sheet = Image.new("RGB", (SIZE * len(frames), SIZE))
    for index, frame in enumerate(frames):
        sheet.paste(frame, (index * SIZE, 0))
    palette = sheet.quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    colors = palette.getpalette()[:384]
    rgb565 = [((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)
              for r, g, b in zip(colors[::3], colors[1::3], colors[2::3])]
    data, offsets = bytearray(), [0]
    for frame in frames:
        pixels = frame.quantize(palette=palette, dither=Image.Dither.NONE).tobytes()
        compressed = zlib.compress(pixels, 9)
        if zlib.decompress(compressed) != pixels or max(pixels) >= 128:
            raise ValueError(f"Invalid frame: {name}")
        data.extend(compressed)
        offsets.append(len(data))
    header = struct.pack("<8s4HI", b"FISHPAK2", SIZE, FPS, len(frames), 128, len(data))
    payload = header + struct.pack("<128H", *rgb565) + struct.pack(f"<{len(offsets)}I", *offsets) + data
    packed.write_bytes(payload)
    frames[len(frames) // 2].save(DESTINATION / f"{identifier}.jpg", quality=88)
    role = "run" if "濂旇窇" in name else "idle" if name == CLIPS[0][1] else "action"
    result = {"id": identifier, "name": name, "role": role, "bytes": len(payload),
              "frames": len(frames), "fps": FPS, "sha256": hashlib.sha256(payload).hexdigest(),
              "source": remote, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
    metadata.write_text(json.dumps(result, ensure_ascii=False) + "\n", encoding="utf-8")
    assert json.loads(metadata.read_text(encoding="utf-8"))["name"] == name
    print(f"Ready {identifier}: {len(frames)} frames, {len(payload)} bytes", flush=True)
    return result


def main():
    DESTINATION.mkdir(parents=True, exist_ok=True)
    sources = ROOT / "assets/library-sources.json"
    if sources.exists():
        catalog = json.loads(sources.read_text(encoding="utf-8"))
        entries = [{"path": clip["source"]} for clip in catalog["clips"]]
    else:
        with urlopen(f"https://api.github.com/repos/Bai-Cute/big-blue-fish-desktop-pet/git/trees/{REVISION}?recursive=1", timeout=60) as response:
            entries = [entry for entry in json.load(response)["tree"]
                       if entry["path"].startswith("code/BigBlueFish.Windows/assets/fish/")]
    entries = [entry for entry in entries if entry["path"].endswith(".webm")]
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        clips = sorted(pool.map(prepare, entries), key=lambda clip: clip["name"])
    defaults = [next(clip["id"] for clip in clips if clip["name"] == original) for _, original in CLIPS]
    catalog = {"revision": REVISION, "size": SIZE, "fps": FPS, "defaults": defaults, "clips": clips}
    path = DESTINATION / "catalog.json"
    path.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    assert len(json.loads(path.read_text(encoding="utf-8"))["clips"]) == len(entries)
    (ROOT / "assets/library-sources.json").write_text(json.dumps(catalog, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print(f"Library verified: {len(clips)} clips, {sum(clip['bytes'] for clip in clips)} packed bytes", flush=True)


if __name__ == "__main__":
    main()
