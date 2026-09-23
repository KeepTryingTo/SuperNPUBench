#!/usr/bin/env python3
"""Run an FP32 GMMA FlashAttention ELF and compare it with a PyTorch golden.

The ELF must be built with ``res_check=on``.  The script parses the FA shape
from the ELF name, writes deterministic Q/K/V inputs into the embedded
``CHK_DIR``, runs the four-PE functional model, and compares ``res.bin`` with

    softmax(Q @ K.T / sqrt(QD)) @ V

Example:

  python3 src/gfrun_fa.py -d /tmp/fa/kernel/fa/elf/kernel_fa_...elf
"""

import argparse
import json
import os
import re
import shlex
import signal
import subprocess
import sys
from pathlib import Path

import numpy as np

try:
    import torch
except ImportError as exc:
    raise SystemExit("gfrun_fa.py requires PyTorch: pip install torch") from exc


SCRIPT_DIR = Path(__file__).resolve().parent
ONE_LEVEL_ROOT = SCRIPT_DIR.parents[3]
COMPARE_ROOT = ONE_LEVEL_ROOT / "compare"
DEFAULT_GFRUN_ROOT = Path(
    "/Users/blacktraker/Programming/gitproj/DV4/SuperScalarModel-asl"
)


def extract_case(elf: Path) -> dict:
    """Extract the static FP32 FA configuration encoded in an ELF name."""
    match = re.search(
        r"fa_2d_unroll_gmma_Sq(?P<Sq>\d+)_Skv(?P<Skv>\d+)"
        r"_Tm(?P<Tm>\d+)_Tk(?P<Tk>\d+)"
        r"(?:_qD(?P<QD>\d+)_vD(?P<VD>\d+))?"
        r"_X(?P<X>\d+)_Y(?P<Y>\d+)_CubeFP32_VectorFP32$",
        elf.stem,
    )
    if not match:
        raise ValueError(
            "expected an fa_2d_unroll_gmma CubeFP32/VectorFP32 ELF name; "
            f"got {elf.name}"
        )
    case = {
        key: int(value)
        for key, value in match.groupdict().items()
        if value is not None
    }
    case.setdefault("QD", 128)
    case.setdefault("VD", 128)
    case["name"] = elf.stem
    return case


def prepare_case(case: dict, args) -> tuple[Path, np.ndarray]:
    """Write deterministic FP32 inputs and a PyTorch FP32 golden."""
    case_dir = COMPARE_ROOT / case["name"]
    case_dir.mkdir(parents=True, exist_ok=True)

    rng = np.random.default_rng(args.seed)

    def make_input(shape):
        data = rng.normal(0.0, args.input_scale, shape)
        return np.clip(data, -1.0, 1.0).astype(np.float32)

    q = make_input((case["Sq"], case["QD"]))
    k = make_input((case["Skv"], case["QD"]))
    v = make_input((case["Skv"], case["VD"]))

    q_t = torch.from_numpy(q)
    k_t = torch.from_numpy(k)
    v_t = torch.from_numpy(v)
    scale = torch.sqrt(torch.tensor(float(case["QD"]), dtype=torch.float32))
    golden = (torch.softmax(torch.matmul(q_t, k_t.T) / scale, dim=-1) @ v_t)
    golden_np = golden.numpy().astype(np.float32, copy=False)

    q.tofile(case_dir / "srcq.bin")
    k.tofile(case_dir / "srck.bin")
    v.tofile(case_dir / "srcv.bin")
    golden_np.tofile(case_dir / "golden.bin")
    np.zeros_like(golden_np).tofile(case_dir / "res.bin")
    return case_dir, golden_np


def run_gfrun(elf: Path, case_dir: Path, args) -> tuple[str, int, str]:
    gfrun_root = args.gfrun_root.expanduser().resolve()
    gfrun = gfrun_root / "bin" / "gfrun"
    if not gfrun.is_file():
        raise ValueError(f"gfrun does not exist: {gfrun}")

    command = [
        str(gfrun),
        "-s",
        "softcore.multiThreadNum=4",
        "-f",
        str(elf),
    ]
    print("command:", shlex.join(command))
    process = subprocess.Popen(
        command,
        cwd=gfrun_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    try:
        output, _ = process.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        (case_dir / "gfrun.log").write_text(output, encoding="utf-8")
        return "TIMEOUT", -1, output

    (case_dir / "gfrun.log").write_text(output, encoding="utf-8")
    reached_end = "Reach the End of Benchmark" in output
    r2_zero = re.search(r"R2\s*=\s*0\b", output) is not None
    passed = process.returncode == 0 and reached_end and r2_zero
    return ("PASS" if passed else "FAIL"), process.returncode, output


def compare_result(
    case_dir: Path, golden: np.ndarray, args
) -> tuple[bool, dict]:
    result_path = case_dir / "res.bin"
    if not result_path.is_file():
        return False, {"reason": "res.bin was not created"}

    result = np.fromfile(result_path, dtype=np.float32)
    if result.size != golden.size:
        return False, {
            "reason": f"shape mismatch: result={result.size}, golden={golden.size}"
        }
    result = result.reshape(golden.shape)

    result64 = result.astype(np.float64)
    golden64 = golden.astype(np.float64)
    diff = result64 - golden64
    abs_diff = np.abs(diff)
    tolerance = args.atol + args.rtol * np.abs(golden64)
    mismatch_mask = abs_diff > tolerance
    max_index = np.unravel_index(np.argmax(abs_diff), abs_diff.shape)
    passed = bool(np.allclose(result, golden, atol=args.atol, rtol=args.rtol))
    metrics = {
        "mse": float(np.mean(diff * diff)),
        "max_abs": float(abs_diff[max_index]),
        "max_index": [int(index) for index in max_index],
        "actual_at_max": float(result[max_index]),
        "golden_at_max": float(golden[max_index]),
        "mismatches": int(np.count_nonzero(mismatch_mask)),
        "elements": int(result.size),
        "all_finite": bool(np.isfinite(result).all()),
        "all_zero": bool(np.all(result == 0)),
    }
    passed = passed and metrics["all_finite"] and not metrics["all_zero"]

    report = case_dir / "golden_compare.log"
    with report.open("w", encoding="utf-8") as stream:
        stream.write(f"status: {'PASS' if passed else 'FAIL'}\n")
        stream.write(f"atol: {args.atol}\nrtol: {args.rtol}\n")
        stream.write(json.dumps(metrics, indent=2, sort_keys=True))
        stream.write("\n\nactual head:\n")
        stream.write(np.array2string(result[:4, :8], precision=9))
        stream.write("\n\ngolden head:\n")
        stream.write(np.array2string(golden[:4, :8], precision=9))
        stream.write("\n")
    metrics["report"] = str(report)
    return passed, metrics


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare FP32 GMMA FlashAttention gfrun output with PyTorch"
    )
    parser.add_argument("-d", "--elf", required=True, help="RES_CHECK FA ELF")
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--input-scale", type=float, default=0.1)
    parser.add_argument("--atol", type=float, default=1e-5)
    parser.add_argument("--rtol", type=float, default=1e-5)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument(
        "--gfrun-root",
        type=Path,
        default=DEFAULT_GFRUN_ROOT,
        help="SuperScalarModel-asl checkout root",
    )
    args = parser.parse_args()

    elf = Path(args.elf).expanduser().resolve()
    if not elf.is_file():
        parser.error(f"ELF does not exist: {elf}")

    try:
        case = extract_case(elf)
        case_dir, golden = prepare_case(case, args)
        run_status, returncode, _ = run_gfrun(elf, case_dir, args)
    except ValueError as exc:
        parser.error(str(exc))

    if run_status != "PASS":
        print(
            f"FAIL: gfrun status={run_status}, returncode={returncode}, "
            f"log={case_dir / 'gfrun.log'}"
        )
        return 1

    passed, metrics = compare_result(case_dir, golden, args)
    print(f"{'PASS' if passed else 'FAIL'}: case={case}")
    print(json.dumps(metrics, indent=2, sort_keys=True))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
