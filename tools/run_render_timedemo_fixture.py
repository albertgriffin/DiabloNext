#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

_TIME_AND_FPS_REGEX = re.compile(
    r"(?P<frames>\d+) frames, (?P<seconds>\d+(?:\.\d+)?) seconds: (?P<fps>\d+(?:\.\d+)?) fps"
)
_SAME_OUTCOME = "Timedemo: Same outcome as initial run. :)"


@dataclass(frozen=True)
class RunMetrics:
    frames: int
    seconds: float
    fps: float
    log_path: Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _resolve_from_cwd(path: str) -> Path:
    candidate = Path(path).expanduser()
    if candidate.is_absolute():
        return candidate.resolve()
    return (Path.cwd() / candidate).resolve()


def _require_file(path: Path, description: str) -> None:
    if not path.is_file():
        raise FileNotFoundError(f"{description} not found: {path}")


def _require_dir(path: Path, description: str) -> None:
    if not path.is_dir():
        raise FileNotFoundError(f"{description} not found: {path}")


def _require_data_file(data_dir: Path, filename: str) -> None:
    expected = filename.lower()
    for child in data_dir.iterdir():
        if child.is_file() and child.name.lower() == expected:
            return
    raise FileNotFoundError(f"{filename} not found in data directory: {data_dir}")


def _copy_required_file(src: Path, dst_dir: Path) -> None:
    _require_file(src, "Fixture file")
    shutil.copy2(src, dst_dir / src.name)


def _prepare_scratch(
    repo_root: Path,
    fixture: Path,
    data_dir: Path,
    scratch_root: Path,
    demo_number: int,
) -> tuple[Path, Path, Path, Path]:
    save_dir = scratch_root / "save"
    config_dir = scratch_root / "config"
    log_dir = scratch_root / "logs"

    for path in (save_dir, config_dir, log_dir):
        if path.exists():
            shutil.rmtree(path)
        path.mkdir(parents=True)

    demo_file = fixture / f"demo_{demo_number}.dmo"
    starting_save = fixture / "single_0.hsv"
    reference_save = fixture / f"demo_{demo_number}_reference_single_0.hsv"
    for file_path in (demo_file, starting_save, reference_save):
        _copy_required_file(file_path, save_dir)

    hellfire_mod_src = repo_root / "mods" / "Hellfire"
    _require_dir(hellfire_mod_src, "Hellfire mod directory")
    (save_dir / "mods").mkdir()
    shutil.copytree(hellfire_mod_src, save_dir / "mods" / "Hellfire")

    local_config = data_dir / "diablo.ini"
    if local_config.is_file():
        shutil.copy2(local_config, config_dir / "diablo.ini")

    return save_dir, config_dir, log_dir, starting_save


def _run_once(
    binary: Path,
    data_dir: Path,
    save_dir: Path,
    config_dir: Path,
    log_dir: Path,
    starting_save: Path,
    demo_number: int,
    run_number: int,
    extra_args: list[str],
) -> RunMetrics:
    shutil.copy2(starting_save, save_dir / "single_0.hsv")

    log_path = log_dir / f"run-{run_number:02}.log"
    command = [
        str(binary),
        "--data-dir",
        str(data_dir),
        "--save-dir",
        str(save_dir),
        "--config-dir",
        str(config_dir),
        "--hellfire",
        "--demo",
        str(demo_number),
        "--timedemo",
        "--log-to-file",
        str(log_path),
        "-n",
        *extra_args,
    ]

    env = os.environ.copy()
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")

    result = subprocess.run(command, capture_output=True, text=True, env=env, check=False)
    captured_output = result.stdout + result.stderr
    log_output = log_path.read_text(errors="replace") if log_path.exists() else ""
    full_output = captured_output + "\n" + log_output

    if result.returncode != 0:
        raise RuntimeError(
            f"Timedemo run {run_number} failed with exit code {result.returncode}; see {log_path}"
        )
    if _SAME_OUTCOME not in full_output:
        raise RuntimeError(
            f"Timedemo run {run_number} did not report same-outcome validation; see {log_path}"
        )

    match = _TIME_AND_FPS_REGEX.search(full_output)
    if match is None:
        raise RuntimeError(f"Timedemo run {run_number} did not report FPS metrics; see {log_path}")

    return RunMetrics(
        frames=int(match.group("frames")),
        seconds=float(match.group("seconds")),
        fps=float(match.group("fps")),
        log_path=log_path,
    )


def _print_summary(metrics: list[RunMetrics]) -> None:
    if len(metrics) == 1:
        run = metrics[0]
        print(f"{run.frames} frames, {run.seconds:.3f} seconds, {run.fps:.3f} FPS")
        return

    seconds = [run.seconds for run in metrics]
    fps = [run.fps for run in metrics]
    print(
        f"{statistics.mean(seconds):.3f} +/- {statistics.stdev(seconds):.3f} seconds, "
        f"{statistics.mean(fps):.3f} +/- {statistics.stdev(fps):.3f} FPS"
    )


def main() -> int:
    repo_root = _repo_root()
    default_fixture = repo_root / "test" / "fixtures" / "timedemo" / "RenderTelemetryHellfireDemo1"

    parser = argparse.ArgumentParser(
        description="Run the preserved Hellfire timedemo fixture and validate same-outcome replay."
    )
    parser.add_argument("--binary", required=True, help="Path to the devilutionx executable")
    parser.add_argument("--data-dir", required=True, help="Path containing Diablo/Hellfire MPQ data")
    parser.add_argument(
        "--fixture",
        default=str(default_fixture),
        help="Timedemo fixture directory",
    )
    parser.add_argument("--scratch-dir", help="Directory for prepared saves, config, and logs")
    parser.add_argument("--runs", type=int, default=1, help="Number of timedemo replays to run")
    parser.add_argument("--demo-number", type=int, default=1, help="Demo number to replay")
    parser.add_argument(
        "extra_args",
        nargs=argparse.REMAINDER,
        help="Additional game arguments after --",
    )
    args = parser.parse_args()

    if args.runs < 1:
        parser.error("--runs must be at least 1")

    binary = _resolve_from_cwd(args.binary)
    data_dir = _resolve_from_cwd(args.data_dir)
    fixture = _resolve_from_cwd(args.fixture)
    scratch_root = (
        _resolve_from_cwd(args.scratch_dir)
        if args.scratch_dir
        else Path(tempfile.mkdtemp(prefix="diablonext-render-timedemo-"))
    )
    extra_args = args.extra_args[1:] if args.extra_args[:1] == ["--"] else args.extra_args

    try:
        _require_file(binary, "Binary")
        _require_dir(data_dir, "Data directory")
        for mpq_name in ("diabdat.mpq", "hellfire.mpq", "hfmonk.mpq", "hfmusic.mpq", "hfvoice.mpq"):
            _require_data_file(data_dir, mpq_name)
        _require_dir(fixture, "Timedemo fixture")
        save_dir, config_dir, log_dir, starting_save = _prepare_scratch(
            repo_root, fixture, data_dir, scratch_root, args.demo_number
        )

        print(f"Scratch directory: {scratch_root}", file=sys.stderr)
        metrics = []
        for run_number in range(1, args.runs + 1):
            print(f"Run {run_number:>2} of {args.runs}: ", end="", file=sys.stderr, flush=True)
            run_metrics = _run_once(
                binary,
                data_dir,
                save_dir,
                config_dir,
                log_dir,
                starting_save,
                args.demo_number,
                run_number,
                extra_args,
            )
            metrics.append(run_metrics)
            print(
                f"{run_metrics.seconds:>6.2f} seconds  {run_metrics.fps:>6.1f} FPS  "
                f"{run_metrics.log_path}",
                file=sys.stderr,
                flush=True,
            )

        _print_summary(metrics)
        return 0
    except (FileNotFoundError, RuntimeError, subprocess.SubprocessError) as err:
        print(f"Error: {err}", file=sys.stderr)
        print(f"Scratch directory: {scratch_root}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
