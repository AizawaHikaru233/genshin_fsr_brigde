import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


FORMAT_NAMES = {
    1: "R32G32B32A32_TYPELESS",
    2: "R32G32B32A32_FLOAT",
    3: "R32G32B32A32_UINT",
    4: "R32G32B32A32_SINT",
    9: "R16G16B16A16_TYPELESS",
    10: "R16G16B16A16_FLOAT",
    19: "R32G8X24_TYPELESS",
    20: "D32_FLOAT_S8X24_UINT",
    21: "R32_FLOAT_X8X24_TYPELESS",
    22: "X32_TYPELESS_G8X24_UINT",
    23: "R10G10B10A2_TYPELESS",
    24: "R10G10B10A2_UNORM",
    25: "R10G10B10A2_UINT",
    26: "R11G11B10_FLOAT",
    48: "R8G8_TYPELESS",
    49: "R8G8_UNORM",
    50: "R8G8_UINT",
    51: "R8G8_SNORM",
    52: "R8G8_SINT",
    54: "R16_FLOAT",
    55: "D16_UNORM",
    56: "R16_UNORM",
    57: "R16_UINT",
    58: "R16_SNORM",
    59: "R16_SINT",
    60: "R8_TYPELESS",
    61: "R8_UNORM",
    62: "R8_UINT",
    63: "R8_SNORM",
    64: "R8_SINT",
}


def load_texture(metadata_path):
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    raw_path = metadata_path.with_suffix(".raw")
    raw = raw_path.read_bytes()
    expected_size = metadata["row_bytes"] * metadata["height"]
    if len(raw) != expected_size:
        raise ValueError(f"{raw_path.name}: expected {expected_size} bytes, got {len(raw)}")

    height = metadata["height"]
    width = metadata["width"]
    view_format = metadata["view_format"]
    resource_format = metadata["resource_format"]

    if view_format == 10 or resource_format == 9:
        return metadata, np.frombuffer(raw, dtype="<f2").reshape(height, width, 4).astype(np.float32)

    if view_format in (2, 3, 4) or resource_format == 1:
        if view_format == 3:
            return metadata, np.frombuffer(raw, dtype="<u4").reshape(height, width, 4)
        if view_format == 4:
            return metadata, np.frombuffer(raw, dtype="<i4").reshape(height, width, 4)
        return metadata, np.frombuffer(raw, dtype="<f4").reshape(height, width, 4)

    if view_format == 26 or resource_format == 26:
        packed = np.frombuffer(raw, dtype="<u4").reshape(height, width)

        def decode_unsigned_float(bits, mantissa_bits):
            exponent = bits >> mantissa_bits
            mantissa = bits & ((1 << mantissa_bits) - 1)
            values = np.empty(bits.shape, dtype=np.float32)
            subnormal = exponent == 0
            special = exponent == 31
            normal = ~(subnormal | special)
            values[subnormal] = np.ldexp(
                mantissa[subnormal].astype(np.float32) / float(1 << mantissa_bits), -14
            )
            values[normal] = np.ldexp(
                1.0 + mantissa[normal].astype(np.float32) / float(1 << mantissa_bits),
                exponent[normal].astype(np.int32) - 15,
            )
            values[special] = np.inf
            values[special & (mantissa != 0)] = np.nan
            return values

        red = decode_unsigned_float(packed & 0x7FF, 6)
        green = decode_unsigned_float((packed >> 11) & 0x7FF, 6)
        blue = decode_unsigned_float((packed >> 22) & 0x3FF, 5)
        return metadata, np.stack((red, green, blue), axis=-1)

    if view_format in (24, 25) or resource_format == 23:
        packed = np.frombuffer(raw, dtype="<u4").reshape(height, width)
        channels = np.stack(
            (
                packed & 0x3FF,
                (packed >> 10) & 0x3FF,
                (packed >> 20) & 0x3FF,
                (packed >> 30) & 0x3,
            ),
            axis=-1,
        ).astype(np.float32)
        if view_format != 25:
            channels /= np.array([1023.0, 1023.0, 1023.0, 3.0], dtype=np.float32)
        return metadata, channels

    if view_format in (20, 21) or resource_format == 19:
        words = np.frombuffer(raw, dtype="<u4").reshape(height, width, 2)
        if view_format == 22:
            return metadata, ((words[..., 1] & 0xFF).astype(np.float32))[..., None]
        return metadata, words[..., 0].view("<f4")[..., None]

    if view_format == 54:
        return metadata, np.frombuffer(raw, dtype="<f2").reshape(height, width, 1).astype(np.float32)

    if view_format in (55, 56) or resource_format == 56:
        values = np.frombuffer(raw, dtype="<u2").reshape(height, width, 1).astype(np.float32)
        if view_format not in (57, 59):
            values /= 65535.0
        return metadata, values

    if view_format in (49, 50, 51, 52) or resource_format == 48:
        values = np.frombuffer(raw, dtype=np.uint8).reshape(height, width, 2)
        if view_format == 51:
            return metadata, np.frombuffer(raw, dtype=np.int8).reshape(height, width, 2).astype(np.float32) / 127.0
        if view_format not in (50, 52):
            values = values.astype(np.float32) / 255.0
        return metadata, values

    if view_format in (61, 62, 63, 64) or resource_format == 60:
        values = np.frombuffer(raw, dtype=np.uint8).reshape(height, width, 1)
        if view_format == 63:
            return metadata, np.frombuffer(raw, dtype=np.int8).reshape(height, width, 1).astype(np.float32) / 127.0
        if view_format not in (62, 64):
            values = values.astype(np.float32) / 255.0
        return metadata, values

    raise ValueError(
        f"unsupported resource/view format {resource_format}/{view_format} for {metadata_path.name}"
    )


def finite_values(channel):
    values = channel[np.isfinite(channel)]
    if values.size == 0:
        raise ValueError("channel contains no finite values")
    return values


def channel_statistics(channel):
    values = finite_values(channel)
    percentiles = np.percentile(values, [0, 0.1, 1, 10, 50, 90, 99, 99.9, 100])
    return {
        "min": float(percentiles[0]),
        "p0_1": float(percentiles[1]),
        "p1": float(percentiles[2]),
        "p10": float(percentiles[3]),
        "median": float(percentiles[4]),
        "p90": float(percentiles[5]),
        "p99": float(percentiles[6]),
        "p99_9": float(percentiles[7]),
        "max": float(percentiles[8]),
        "mean": float(np.mean(values)),
        "stddev": float(np.std(values)),
        "finite_fraction": float(values.size / channel.size),
        "zero_fraction": float(np.count_nonzero(values == 0) / values.size),
        "one_fraction": float(np.count_nonzero(values == 1) / values.size),
    }


def normalize_channel(channel):
    values = finite_values(channel)
    low, high = np.percentile(values, [1, 99])
    if not np.isfinite(low) or not np.isfinite(high) or high <= low:
        low = float(np.min(values))
        high = float(np.max(values))
    if high <= low:
        return np.zeros(channel.shape, dtype=np.uint8)
    normalized = np.nan_to_num((channel - low) / (high - low), nan=0.0, posinf=1.0, neginf=0.0)
    return np.round(np.clip(normalized, 0.0, 1.0) * 255.0).astype(np.uint8)


def save_grayscale(path, channel):
    Image.fromarray(normalize_channel(channel), mode="L").save(path)


def save_color(path, channels):
    color = np.clip(channels[..., :3], 0.0, None)
    high = float(np.percentile(color[np.isfinite(color)], 99.9))
    if high > 1.0:
        color /= high
    color = np.power(np.clip(color, 0.0, 1.0), 1.0 / 2.2)
    Image.fromarray(np.round(color * 255.0).astype(np.uint8), mode="RGB").save(path)


def save_motion_visualizations(output_dir, channels):
    encoded = channels[..., :2]
    centered = encoded - (127.0 / 255.0)
    decoded = np.sign(centered) * np.square(np.abs(centered) * 2.0)
    magnitude = np.linalg.norm(decoded, axis=-1)
    scale = float(np.percentile(np.abs(decoded), 99.5))
    if scale <= 0.0:
        scale = 1.0
    visualization = np.zeros((*decoded.shape[:2], 3), dtype=np.float32)
    visualization[..., 0] = np.clip(decoded[..., 0] / (2.0 * scale) + 0.5, 0.0, 1.0)
    visualization[..., 1] = np.clip(decoded[..., 1] / (2.0 * scale) + 0.5, 0.0, 1.0)
    visualization[..., 2] = np.clip(magnitude / (np.sqrt(2.0) * scale), 0.0, 1.0)
    Image.fromarray(np.round(visualization * 255.0).astype(np.uint8), mode="RGB").save(
        output_dir / "t3_motion_decoded.png"
    )
    save_grayscale(output_dir / "t3_motion_magnitude.png", magnitude)
    return {
        "decode": "sign(encoded_xy - 127/255) * (abs(encoded_xy - 127/255) * 2)^2",
        "x": channel_statistics(decoded[..., 0]),
        "y": channel_statistics(decoded[..., 1]),
        "magnitude": channel_statistics(magnitude),
        "encoded_center": 127.0 / 255.0,
    }


def analyze(input_dir, output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    results = {"input_directory": str(input_dir.resolve()), "slots": {}}
    metadata_paths = sorted(input_dir.glob("t[0-7].json"))
    if not metadata_paths:
        raise FileNotFoundError(f"no t0-t6 metadata found in {input_dir}")

    for metadata_path in metadata_paths:
        try:
            metadata, channels = load_texture(metadata_path)
        except ValueError as error:
            results["slots"][metadata_path.stem] = {"error": str(error)}
            continue
        slot_name = f"t{metadata['slot']}"
        slot_result = {
            **metadata,
            "resource_format_name": FORMAT_NAMES.get(metadata["resource_format"], "UNKNOWN"),
            "view_format_name": FORMAT_NAMES.get(metadata["view_format"], "UNKNOWN"),
            "channels": [channel_statistics(channels[..., index]) for index in range(channels.shape[-1])],
        }
        results["slots"][slot_name] = slot_result

        if slot_name in ("t0", "t6", "t7") and channels.shape[-1] >= 3:
            save_color(output_dir / f"{slot_name}_color.png", channels)
        for index in range(channels.shape[-1]):
            save_grayscale(output_dir / f"{slot_name}_channel_{index}.png", channels[..., index])
        if slot_name == "t3" and channels.shape[-1] >= 2:
            slot_result["decoded_motion"] = save_motion_visualizations(output_dir, channels)

    cb0_metadata_path = input_dir / "cb0.json"
    cb0_raw_path = input_dir / "cb0.raw"
    if cb0_metadata_path.exists() and cb0_raw_path.exists():
        cb0_metadata = json.loads(cb0_metadata_path.read_text(encoding="utf-8"))
        cb0 = np.frombuffer(cb0_raw_path.read_bytes(), dtype="<f4")
        if cb0.size != cb0_metadata["float_count"]:
            raise ValueError(f"cb0.raw: expected {cb0_metadata['float_count']} floats, got {cb0.size}")
        float4_count = cb0.size // 4
        cb0_float4 = cb0[: float4_count * 4].reshape(float4_count, 4)
        results["cb0"] = {
            **cb0_metadata,
            "float4": {str(index): [float(value) for value in cb0_float4[index]] for index in range(float4_count)},
        }
        if float4_count > 28:
            results["cb0"]["target_parameters"] = {
                "cb26_render_size_candidate": [float(value) for value in cb0_float4[26]],
                "cb27_size_and_reciprocal_candidate": [float(value) for value in cb0_float4[27]],
                "cb28_jitter_candidate": [float(value) for value in cb0_float4[28]],
            }

    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")

    report_lines = []
    for slot_name, slot in results["slots"].items():
        if "error" in slot:
            report_lines.append(f"{slot_name}: skipped ({slot['error']})")
            continue
        report_lines.append(
            f"{slot_name}: {slot['width']}x{slot['height']} "
            f"resource={slot['resource_format_name']}({slot['resource_format']}) "
            f"view={slot['view_format_name']}({slot['view_format']})"
        )
        for index, channel in enumerate(slot["channels"]):
            report_lines.append(
                f"  c{index}: min={channel['min']:.8g} median={channel['median']:.8g} "
                f"max={channel['max']:.8g} mean={channel['mean']:.8g} std={channel['stddev']:.8g}"
            )
        if "decoded_motion" in slot:
            motion = slot["decoded_motion"]
            report_lines.append(
                f"  decoded motion: x median={motion['x']['median']:.8g} std={motion['x']['stddev']:.8g}; "
                f"y median={motion['y']['median']:.8g} std={motion['y']['stddev']:.8g}; "
                f"magnitude p99={motion['magnitude']['p99']:.8g}"
            )
    if "cb0" in results and "target_parameters" in results["cb0"]:
        target_parameters = results["cb0"]["target_parameters"]
        report_lines.append(f"cb26: {target_parameters['cb26_render_size_candidate']}")
        report_lines.append(f"cb27: {target_parameters['cb27_size_and_reciprocal_candidate']}")
        report_lines.append(f"cb28: {target_parameters['cb28_jitter_candidate']}")
    (output_dir / "report.txt").write_text("\n".join(report_lines) + "\n", encoding="utf-8")
    return summary_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    arguments = parser.parse_args()
    output_dir = arguments.output_dir or arguments.input_dir / "analysis"
    summary_path = analyze(arguments.input_dir, output_dir)
    print(summary_path)


if __name__ == "__main__":
    main()
