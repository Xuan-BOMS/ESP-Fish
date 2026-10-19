"""Shared VP9 decoding and device animation settings."""
from pathlib import Path

import imageio_ffmpeg
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
REVISION = "ddc63d9d8aa0701282e68cc6f3fdc26d5c81e793"
BASE = f"https://raw.githubusercontent.com/Bai-Cute/big-blue-fish-desktop-pet/{REVISION}/"
CLIPS = [("idle", "待机呼吸休闲"), ("run", "原地左转奔跑"),
         ("wave", "点击回应-元气挥手"), ("bubble", "鲸鱼吐泡泡特效")]
SIZE, FPS = 168, 6
BACKGROUND = (8, 20, 41, 255)


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
