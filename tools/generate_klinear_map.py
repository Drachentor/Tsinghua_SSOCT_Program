#!/usr/bin/env python3
"""
Generate klinear_resample_indices.txt from two mirror calibration measurements.

The algorithm follows the two-sided mirror phase method described by
Attendu et al., JBO 24(5), 056001 (2019):

    phase_linear = (phase_positive + phase_negative) / 2

where the two mirror measurements are acquired on opposite sides of the
zero-delay plane. The resulting phase contains the k-sampling nonlinearity
while canceling the dispersion phase term. The script inverts that phase curve
to produce fractional source indices for spectral interpolation.

Important: all A-lines in the positive set should be repeated measurements at
one fixed mirror position, and all A-lines in the negative set should be
repeated measurements at one fixed mirror position. Do not average A-lines from
different mirror depths in the same set.

Typical use:

    python tools/generate_klinear_map.py ^
        --positive mirror_pos.bin --negative mirror_neg.bin ^
        --ascan-len 640 --dtype float32 ^
        --swept-source-id Thorlabs_SL_134051

For manually acquired single A-lines, pass folders, wildcards, or multiple
files:

    python tools/generate_klinear_map.py ^
        --positive mirror_pos/*.txt --negative mirror_neg/*.txt ^
        --ascan-len 1600 --dtype text ^
        --swept-source-id Thorlabs_SL_134051

For files saved by the application, --ascan-len and --dtype can usually be
omitted if the same-name acquisition JSON sidecar is present.

If the files are raw uint16 data saved directly from the ADC, use
--dtype uint16. By default uint16 values are shifted right by 4 bits to match
the application processing path; pass --raw-shift-bits 0 if the data is not
left-aligned.
"""

from __future__ import annotations

import argparse
import glob
import json
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

import numpy as np


KLINEAR_OUTPUT_NAME = "klinear_resample_indices.txt"
KLINEAR_DIAGNOSTICS_NAME = "klinear_resample_diagnostics.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate OCT software k-linearization resampling indices."
    )
    parser.add_argument("--positive", nargs="+", required=True,
                        help="Repeated A-lines at one fixed mirror position on one side of zero-delay. Accepts files, folders, or wildcards.")
    parser.add_argument("--negative", nargs="+", required=True,
                        help="Repeated A-lines at one fixed mirror position on the opposite side of zero-delay. Accepts files, folders, or wildcards.")
    parser.add_argument("--ascan-len", type=int,
                        help="Number of samples in one A-line. If omitted, read from sidecar JSON.")
    parser.add_argument("--dtype", choices=("float32", "float64", "uint16", "text", "npy"),
                        help="Input file format. If omitted, read from sidecar JSON or infer from suffix.")
    parser.add_argument("--sidecar", type=Path,
                        help="Optional acquisition JSON sidecar used to infer AscanLen and dtype.")
    parser.add_argument("--background", nargs="+",
                        help="Optional background spectrum/data applied to both inputs. Accepts files, folders, or wildcards.")
    parser.add_argument("--positive-background", nargs="+",
                        help="Optional background applied only to the positive input. Accepts files, folders, or wildcards.")
    parser.add_argument("--negative-background", nargs="+",
                        help="Optional background applied only to the negative input. Accepts files, folders, or wildcards.")
    parser.add_argument("--background-dtype",
                        choices=("float32", "float64", "uint16", "text", "npy"),
                        help="Background format. Defaults to --dtype.")
    parser.add_argument("--raw-shift-bits", type=int, default=4,
                        help="Right shift applied to uint16 samples before averaging.")
    parser.add_argument("--max-lines", type=int, default=0,
                        help="Use only the first N A-lines from each input. 0 means all.")
    parser.add_argument("--trim-left", type=int, default=5,
                        help="Samples omitted at the beginning before fitting.")
    parser.add_argument("--trim-right", type=int, default=5,
                        help="Samples omitted at the end before fitting.")
    parser.add_argument("--poly-degree", type=int, default=5,
                        help="Polynomial degree for smoothing phase versus source index.")
    parser.add_argument("--high-pass-samples", type=int, default=0,
                        help="Subtract a moving average with this width before phase extraction.")
    parser.add_argument("--keep-dc", action="store_true",
                        help="Do not subtract the mean value from each averaged spectrum.")
    parser.add_argument("--output", type=Path,
                        help="Output resampling index text file. Defaults to parameters/calibration/<swept-source-id>/ascan_<N>/klinear_resample_indices.txt.")
    parser.add_argument("--diagnostics", type=Path,
                        help="JSON diagnostics output path. Defaults to klinear_resample_diagnostics.json next to --output.")
    parser.add_argument("--swept-source-id",
                        help="Swept-source/laser id written to diagnostics. Defaults to same-name sidecar metadata when present.")
    parser.add_argument("--swept-source-name",
                        help="Swept-source/laser display name written to diagnostics.")
    return parser.parse_args()


def safe_path_segment(value: str, fallback: str) -> str:
    text = (value or "").strip() or fallback
    segment = "".join(char if char.isalnum() or char in "._-" else "_" for char in text)
    return segment or fallback


def default_output_path(swept_source_id: str, ascan_len: int) -> Path:
    source_segment = safe_path_segment(swept_source_id, "unknown_swept_source")
    return Path("parameters") / "calibration" / source_segment / f"ascan_{ascan_len}" / KLINEAR_OUTPUT_NAME


def dtype_for_numpy(name: str) -> np.dtype:
    if name == "float32":
        return np.dtype("<f4")
    if name == "float64":
        return np.dtype("<f8")
    if name == "uint16":
        return np.dtype("<u2")
    raise ValueError(f"{name} is not a binary dtype")


def has_wildcard(value: str) -> bool:
    return any(char in value for char in "*?[]")


def expand_path_patterns(patterns: Optional[Sequence[str]]) -> List[Path]:
    if not patterns:
        return []

    paths: List[Path] = []
    for pattern in patterns:
        if has_wildcard(pattern):
            matches = [Path(value) for value in glob.glob(pattern)]
            if not matches:
                raise FileNotFoundError(f"No files matched pattern: {pattern}")
            paths.extend(matches)
            continue

        path = Path(pattern)
        if path.is_dir():
            paths.extend(
                child for child in path.iterdir()
                if child.is_file() and child.suffix.lower() != ".json"
            )
        else:
            paths.append(path)

    unique_paths = sorted({path.resolve(): path for path in paths}.values(), key=lambda item: str(item))
    if not unique_paths:
        raise FileNotFoundError("No input files were provided")
    return unique_paths


def sidecar_path_for_data_file(path: Path) -> Path:
    return path.with_name(path.stem + ".json")


def sample_type_to_dtype(sample_type: str) -> Optional[str]:
    normalized = sample_type.strip().lower()
    if normalized == "uint16_raw":
        return "uint16"
    if normalized == "float32_spectrum":
        return "float32"
    return None


def read_sidecar_metadata(path: Optional[Path]) -> dict:
    if path is None or not path.exists():
        return {}

    with path.open("r", encoding="utf-8") as handle:
        root = json.load(handle)

    metadata = {}
    acquisition = root.get("acquisition", {})
    if isinstance(acquisition, dict):
        if "ascanLen" in acquisition:
            metadata["ascan_len"] = int(acquisition["ascanLen"])
        dtype_name = sample_type_to_dtype(str(acquisition.get("sampleType", "")))
        if dtype_name:
            metadata["dtype"] = dtype_name
        if "sweptSourceId" in acquisition:
            metadata["swept_source_id"] = str(acquisition["sweptSourceId"])
        if "sweptSourceName" in acquisition:
            metadata["swept_source_name"] = str(acquisition["sweptSourceName"])

    settings = root.get("settings", {})
    if "ascan_len" not in metadata and isinstance(settings, dict):
        main_settings = settings.get("mainWidget", {})
        if isinstance(main_settings, dict) and "AscanLen" in main_settings:
            metadata["ascan_len"] = int(main_settings["AscanLen"])
        devices = settings.get("devices", {})
        if isinstance(devices, dict) and devices.get("selectedSweptSourceId"):
            metadata["swept_source_id"] = str(devices["selectedSweptSourceId"])
            metadata["swept_source_name"] = str(devices.get("selectedSweptSourceName", ""))

    return metadata


def resolve_input_metadata(args: argparse.Namespace,
                           positive_paths: Sequence[Path]) -> Tuple[int, str, Optional[Path], dict]:
    sidecar = args.sidecar
    if sidecar is None and positive_paths:
        candidate = sidecar_path_for_data_file(positive_paths[0])
        if candidate.exists():
            sidecar = candidate
    metadata = read_sidecar_metadata(sidecar)

    ascan_len = args.ascan_len or metadata.get("ascan_len")
    if ascan_len is None:
        raise ValueError(
            "AscanLen is required. Pass --ascan-len or provide a same-name acquisition JSON sidecar."
        )
    if ascan_len <= 0:
        raise ValueError("AscanLen must be positive")

    dtype_name = args.dtype or metadata.get("dtype")
    if dtype_name is None:
        suffix = positive_paths[0].suffix.lower() if positive_paths else ""
        dtype_name = "uint16" if suffix in {".2d", ".3d"} else "float32"
    return int(ascan_len), dtype_name, sidecar, metadata


def load_measurement_file(path: Path,
                          ascan_len: int,
                          dtype_name: str,
                          raw_shift_bits: int) -> np.ndarray:
    if ascan_len <= 0:
        raise ValueError("ascan_len must be positive")
    if not path.exists():
        raise FileNotFoundError(path)

    if dtype_name == "npy":
        values = np.load(path)
    elif dtype_name == "text":
        values = np.loadtxt(path, dtype=np.float64)
    else:
        values = np.fromfile(path, dtype=dtype_for_numpy(dtype_name))

    values = np.asarray(values)
    if values.size == 0:
        raise ValueError(f"{path} is empty")

    if dtype_name == "uint16":
        if raw_shift_bits < 0:
            raise ValueError("raw_shift_bits must be non-negative")
        if raw_shift_bits > 0:
            values = np.right_shift(values.astype(np.uint16), raw_shift_bits)
        values = values.astype(np.float64)
    else:
        values = values.astype(np.float64, copy=False)

    if values.ndim == 1:
        if values.size % ascan_len != 0:
            raise ValueError(
                f"{path} has {values.size} values, not a multiple of AscanLen={ascan_len}"
            )
        values = values.reshape((-1, ascan_len))
    elif values.ndim == 2:
        if values.shape[1] == ascan_len:
            pass
        elif values.shape[0] == ascan_len:
            values = values.T
        else:
            raise ValueError(
                f"{path} shape {values.shape} does not contain AscanLen={ascan_len}"
            )
    else:
        raise ValueError(f"{path} must contain a 1D or 2D array")

    if values.shape[0] <= 0:
        raise ValueError(f"{path} contains no A-lines")
    return values


def load_measurements(paths: Sequence[Path],
                      ascan_len: int,
                      dtype_name: str,
                      raw_shift_bits: int,
                      max_lines: int) -> np.ndarray:
    if not paths:
        raise FileNotFoundError("No input files were provided")

    measurements = [
        load_measurement_file(path, ascan_len, dtype_name, raw_shift_bits)
        for path in paths
    ]
    values = np.vstack(measurements)
    if max_lines > 0:
        values = values[:max_lines, :]
    if values.shape[0] <= 0:
        raise ValueError("No A-lines remain after max-lines filtering")
    return values


def load_average(paths: Sequence[Path],
                 ascan_len: int,
                 dtype_name: str,
                 raw_shift_bits: int,
                 max_lines: int) -> Optional[np.ndarray]:
    if not paths:
        return None
    measurements = load_measurements(paths, ascan_len, dtype_name, raw_shift_bits, max_lines)
    return np.mean(measurements, axis=0)


def moving_average_reflect(values: np.ndarray, width: int) -> np.ndarray:
    if width <= 1:
        return values.copy()
    width = min(width, values.size)
    if width <= 1:
        return values.copy()

    left = width // 2
    right = width - 1 - left
    padded = np.pad(values, (left, right), mode="reflect")
    kernel = np.ones(width, dtype=np.float64) / float(width)
    return np.convolve(padded, kernel, mode="valid")


def preprocess_average(measurements: np.ndarray,
                       background: Optional[np.ndarray],
                       high_pass_samples: int,
                       keep_dc: bool) -> np.ndarray:
    spectrum = np.mean(measurements, axis=0)
    if background is not None:
        if background.shape != spectrum.shape:
            raise ValueError(
                f"background length {background.size} does not match AscanLen={spectrum.size}"
            )
        spectrum = spectrum - background
    if not keep_dc:
        spectrum = spectrum - np.mean(spectrum)
    if high_pass_samples > 1:
        spectrum = spectrum - moving_average_reflect(spectrum, high_pass_samples)
    return spectrum


def analytic_signal(real_signal: np.ndarray) -> np.ndarray:
    n = real_signal.size
    spectrum = np.fft.fft(real_signal)
    hilbert_filter = np.zeros(n, dtype=np.float64)
    if n % 2 == 0:
        hilbert_filter[0] = 1.0
        hilbert_filter[n // 2] = 1.0
        hilbert_filter[1:n // 2] = 2.0
    else:
        hilbert_filter[0] = 1.0
        hilbert_filter[1:(n + 1) // 2] = 2.0
    return np.fft.ifft(spectrum * hilbert_filter)


def unwrapped_phase(real_signal: np.ndarray) -> np.ndarray:
    signal = np.asarray(real_signal, dtype=np.float64)
    if not np.all(np.isfinite(signal)):
        raise ValueError("input spectrum contains NaN or inf")
    phase = np.unwrap(np.angle(analytic_signal(signal)))
    if phase[-1] < phase[0]:
        phase = -phase
    return phase


def fit_monotonic_phase(indices: np.ndarray,
                        phase: np.ndarray,
                        degree: int) -> Tuple[np.ndarray, np.ndarray, dict]:
    requested_degree = max(1, min(int(degree), indices.size - 1))
    attempts = []

    for candidate_degree in range(requested_degree, 0, -1):
        coefficients = np.polyfit(indices, phase, candidate_degree)
        fitted = np.polyval(coefficients, indices)

        if fitted[-1] < fitted[0]:
            fitted = -fitted
            coefficients = -coefficients

        deltas = np.diff(fitted)
        min_delta = float(np.min(deltas))
        attempts.append({
            "degree": int(candidate_degree),
            "min_delta": min_delta,
        })
        if min_delta > 0.0:
            diagnostics = {
                "requested_poly_degree": int(requested_degree),
                "selected_poly_degree": int(candidate_degree),
                "poly_degree_auto_downgraded": bool(candidate_degree != requested_degree),
                "fit_attempts": attempts,
                "fit_min_delta": min_delta,
            }
            return fitted, coefficients, diagnostics

    phase_slope = float(phase[-1] - phase[0])
    raise ValueError(
        "fitted phase is not strictly increasing even after trying lower polynomial degrees. "
        f"valid phase slope={phase_slope:g}; attempts={attempts}. "
        "Please verify that each input set is repeated A-lines at one fixed mirror position, "
        "that the two files are on opposite sides of zero-delay, and that the spectra contain "
        "clear mirror fringes. You can also try larger --trim-left/--trim-right values or "
        "add --high-pass-samples 101."
    )


def generate_resample_indices(positive: np.ndarray,
                              negative: np.ndarray,
                              trim_left: int,
                              trim_right: int,
                              poly_degree: int) -> Tuple[np.ndarray, dict]:
    if positive.shape != negative.shape:
        raise ValueError("positive and negative spectra must have the same length")

    ascan_len = positive.size
    if trim_left < 0 or trim_right < 0 or trim_left + trim_right >= ascan_len - 2:
        raise ValueError("trim values leave too few phase samples")

    phase_positive = unwrapped_phase(positive)
    phase_negative = unwrapped_phase(negative)
    phase_linear = 0.5 * (phase_positive + phase_negative)
    if phase_linear[-1] < phase_linear[0]:
        phase_linear = -phase_linear

    valid_start = trim_left
    valid_end = ascan_len - trim_right
    valid_indices = np.arange(valid_start, valid_end, dtype=np.float64)
    valid_phase = phase_linear[valid_start:valid_end]

    fitted_phase, coefficients, fit_diagnostics = fit_monotonic_phase(valid_indices,
                                                                      valid_phase,
                                                                      poly_degree)
    target_phase = np.linspace(fitted_phase[0], fitted_phase[-1], ascan_len)
    resample_indices = np.interp(target_phase, fitted_phase, valid_indices)

    identity = np.arange(ascan_len, dtype=np.float64)
    correction = resample_indices - identity
    diagnostics = {
        "ascan_len": int(ascan_len),
        "trim_left": int(trim_left),
        "trim_right": int(trim_right),
        "poly_degree": int(fit_diagnostics["selected_poly_degree"]),
        "requested_poly_degree": int(fit_diagnostics["requested_poly_degree"]),
        "poly_degree_auto_downgraded": bool(fit_diagnostics["poly_degree_auto_downgraded"]),
        "fit_attempts": fit_diagnostics["fit_attempts"],
        "fit_min_delta": float(fit_diagnostics["fit_min_delta"]),
        "phase_slope_positive": float(phase_positive[-1] - phase_positive[0]),
        "phase_slope_negative": float(phase_negative[-1] - phase_negative[0]),
        "phase_slope_linear": float(phase_linear[-1] - phase_linear[0]),
        "resample_min": float(np.min(resample_indices)),
        "resample_max": float(np.max(resample_indices)),
        "correction_rms_samples": float(np.sqrt(np.mean(correction * correction))),
        "correction_max_abs_samples": float(np.max(np.abs(correction))),
        "poly_coefficients_high_to_low": [float(value) for value in coefficients],
    }
    return resample_indices, diagnostics


def write_resample_indices(path: Path, indices: np.ndarray, diagnostics: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("# k-linearization source indices generated by tools/generate_klinear_map.py\n")
        handle.write(f"# ascan_len: {diagnostics['ascan_len']}\n")
        handle.write(f"# correction_rms_samples: {diagnostics['correction_rms_samples']:.9g}\n")
        handle.write(f"# correction_max_abs_samples: {diagnostics['correction_max_abs_samples']:.9g}\n")
        for value in indices:
            handle.write(f"{value:.9f}\n")


def main() -> int:
    args = parse_args()
    positive_paths = expand_path_patterns(args.positive)
    negative_paths = expand_path_patterns(args.negative)
    common_background_paths = expand_path_patterns(args.background) if args.background else []
    positive_background_paths = (
        expand_path_patterns(args.positive_background)
        if args.positive_background else common_background_paths
    )
    negative_background_paths = (
        expand_path_patterns(args.negative_background)
        if args.negative_background else common_background_paths
    )

    ascan_len, dtype_name, sidecar, sidecar_metadata = resolve_input_metadata(args, positive_paths)
    swept_source_id = args.swept_source_id or sidecar_metadata.get("swept_source_id", "")
    if not swept_source_id:
        raise ValueError(
            "Cannot determine swept source id. Pass --swept-source-id or provide an acquisition JSON sidecar."
        )
    swept_source_name = (
        args.swept_source_name
        or sidecar_metadata.get("swept_source_name", "")
        or swept_source_id
    )
    output_path = args.output or default_output_path(swept_source_id, ascan_len)
    diagnostics_path = args.diagnostics or output_path.with_name(KLINEAR_DIAGNOSTICS_NAME)
    background_dtype = args.background_dtype or dtype_name

    positive_background = load_average(
        positive_background_paths,
        ascan_len,
        background_dtype,
        args.raw_shift_bits,
        args.max_lines,
    )
    negative_background = load_average(
        negative_background_paths,
        ascan_len,
        background_dtype,
        args.raw_shift_bits,
        args.max_lines,
    )

    positive_measurements = load_measurements(
        positive_paths,
        ascan_len,
        dtype_name,
        args.raw_shift_bits,
        args.max_lines,
    )
    negative_measurements = load_measurements(
        negative_paths,
        ascan_len,
        dtype_name,
        args.raw_shift_bits,
        args.max_lines,
    )

    positive_spectrum = preprocess_average(
        positive_measurements,
        positive_background,
        args.high_pass_samples,
        args.keep_dc,
    )
    negative_spectrum = preprocess_average(
        negative_measurements,
        negative_background,
        args.high_pass_samples,
        args.keep_dc,
    )

    indices, diagnostics = generate_resample_indices(
        positive_spectrum,
        negative_spectrum,
        args.trim_left,
        args.trim_right,
        args.poly_degree,
    )
    diagnostics.update({
        "averaging_assumption": (
            "positive A-lines are repeated at one fixed mirror position; "
            "negative A-lines are repeated at one fixed mirror position"
        ),
        "positive_files": [str(path) for path in positive_paths],
        "negative_files": [str(path) for path in negative_paths],
        "positive_background_files": [str(path) for path in positive_background_paths],
        "negative_background_files": [str(path) for path in negative_background_paths],
        "sidecar": str(sidecar) if sidecar else "",
        "positive_input_file_count": int(len(positive_paths)),
        "negative_input_file_count": int(len(negative_paths)),
        "positive_lines": int(positive_measurements.shape[0]),
        "negative_lines": int(negative_measurements.shape[0]),
        "dtype": dtype_name,
        "background_dtype": background_dtype,
        "raw_shift_bits": int(args.raw_shift_bits),
        "high_pass_samples": int(args.high_pass_samples),
        "keep_dc": bool(args.keep_dc),
        "swept_source_id": swept_source_id,
        "swept_source_name": swept_source_name,
        "output": str(output_path),
    })

    write_resample_indices(output_path, indices, diagnostics)
    diagnostics_path.parent.mkdir(parents=True, exist_ok=True)
    diagnostics_path.write_text(
        json.dumps(diagnostics, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    print(f"Wrote {indices.size} resampling indices to {output_path}")
    print(f"Wrote diagnostics to {diagnostics_path}")
    if diagnostics.get("poly_degree_auto_downgraded"):
        print(
            "Polynomial fit was auto-downgraded from degree "
            f"{diagnostics['requested_poly_degree']} to {diagnostics['poly_degree']} "
            "to keep the phase curve strictly increasing."
        )
    print(
        "RMS correction: "
        f"{diagnostics['correction_rms_samples']:.4g} samples; "
        "max correction: "
        f"{diagnostics['correction_max_abs_samples']:.4g} samples"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
