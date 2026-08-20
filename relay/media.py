"""Media transcoding: iPhone formats in, ESP32-native formats out.

The whole point of this module is that the firmware should never have to
decode anything. Photos leave here as raw RGB565 the panel can eat
directly; audio leaves as 16 kHz mono PCM, which is the ES8311's native
rate in both directions.
"""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

import numpy as np
import pillow_heif
from PIL import Image, ImageOps

pillow_heif.register_heif_opener()

PANEL_W = 368
PANEL_H = 448
SAMPLE_RATE = 16000


class MediaError(RuntimeError):
    pass


def _run(cmd: list[str]) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise MediaError(f"{cmd[0]} failed: {proc.stderr.strip()[:400]}")
    return proc.stdout


def photo_to_rgb565(src: Path, dst: Path, big_endian: bool = True) -> int:
    """Decode anything Pillow understands (incl. HEIC) into panel-ready pixels."""
    try:
        with Image.open(src) as im:
            im = ImageOps.exif_transpose(im)
            im = im.convert("RGB")
            # Crop biased slightly above centre: phone photos of people put
            # faces in the upper half, and a dead-centre crop beheads them.
            im = ImageOps.fit(
                im, (PANEL_W, PANEL_H), method=Image.LANCZOS, centering=(0.5, 0.4)
            )
            arr = np.asarray(im, dtype=np.uint16)
    except MediaError:
        raise
    except Exception as exc:
        raise MediaError(f"could not read image: {exc}") from exc

    r = (arr[:, :, 0] >> 3).astype(np.uint16)
    g = (arr[:, :, 1] >> 2).astype(np.uint16)
    b = (arr[:, :, 2] >> 3).astype(np.uint16)
    packed = (r << 11) | (g << 5) | b
    packed.astype(">u2" if big_endian else "<u2").tofile(dst)

    size = dst.stat().st_size
    expected = PANEL_W * PANEL_H * 2
    if size != expected:
        raise MediaError(f"rgb565 output was {size} bytes, expected {expected}")
    return size


def photo_preview_jpeg(src: Path, dst: Path) -> int:
    """Small JPEG kept only so humans can eyeball what the device received."""
    with Image.open(src) as im:
        im = ImageOps.exif_transpose(im).convert("RGB")
        im = ImageOps.fit(im, (PANEL_W, PANEL_H), method=Image.LANCZOS,
                          centering=(0.5, 0.4))
        im.save(dst, "JPEG", quality=82, optimize=True)
    return dst.stat().st_size


def audio_to_wav16k(src: Path, dst: Path, max_sec: int = 60) -> int:
    """Transcode to what the codec wants, and even out the volume.

    loudnorm matters more than it looks: voice notes get recorded in
    airports, hotel rooms and taxis at wildly different levels, and a
    five-year-old is not going to work the volume slider.
    """
    _run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(src),
        "-t", str(max_sec),
        "-vn",
        "-ac", "1",
        "-ar", str(SAMPLE_RATE),
        "-af", "loudnorm=I=-16:TP=-1.5:LRA=11",
        "-c:a", "pcm_s16le",
        str(dst),
    ])
    return dst.stat().st_size


def wav_to_m4a(src: Path, dst: Path) -> int:
    """Her reply, packaged for the iPhone's notification player."""
    _run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(src), "-c:a", "aac", "-b:a", "64k", str(dst),
    ])
    return dst.stat().st_size


def duration_ms(path: Path) -> int:
    out = _run([
        "ffprobe", "-hide_banner", "-loglevel", "error",
        "-show_entries", "format=duration", "-of", "json", str(path),
    ])
    try:
        return int(float(json.loads(out)["format"]["duration"]) * 1000)
    except Exception:
        return 0
