import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def convert_capture(metadata_path: Path) -> Path:
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    width = int(metadata["width"])
    height = int(metadata["height"])
    raw_path = metadata_path.with_suffix(".raw")
    pixels = np.fromfile(raw_path, dtype="<f2").reshape(height, width, 4)[..., :3].astype(np.float32)

    luminance = pixels[..., 0] * 0.2126 + pixels[..., 1] * 0.7152 + pixels[..., 2] * 0.0722
    positive = luminance[np.isfinite(luminance) & (luminance > 0)]
    white = float(np.percentile(positive, 99.5)) if positive.size else 1.0
    scale = 1.0 / max(white, 1e-6)
    mapped = np.maximum(pixels * scale, 0.0)
    mapped = mapped / (1.0 + mapped)
    srgb = np.where(
        mapped <= 0.0031308,
        mapped * 12.92,
        1.055 * np.power(mapped, 1.0 / 2.4) - 0.055,
    )
    preview = np.clip(srgb * 255.0 + 0.5, 0, 255).astype(np.uint8)
    output_path = metadata_path.with_suffix(".png")
    Image.fromarray(preview, "RGB").save(output_path)
    return output_path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    arguments = parser.parse_args()
    for metadata_path in sorted(arguments.directory.glob("pre_color_*.json")):
        print(convert_capture(metadata_path))


if __name__ == "__main__":
    main()
