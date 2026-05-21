# Local Platform Testing

This page documents repeatable local build and render timedemo checks from an
Apple Silicon MacBook. The goal is fast local confidence before relying on
GitHub Actions for broader platform coverage.

## Data Directory

The Hellfire timedemo runner needs a directory containing the Diablo and
Hellfire MPQs:

```bash
DATA_DIR="$HOME/Library/Application Support/diasurgical/devilution"
```

The runner validates that `diabdat.mpq`, `hellfire.mpq`, `hfmonk.mpq`,
`hfmusic.mpq`, and `hfvoice.mpq` are present. File name case does not matter.

## Local Commit Formatting

Install the versioned pre-commit hook once per clone:

```bash
tools/install_git_hooks.sh
```

This sets `core.hooksPath` to `.githooks`. Linked worktrees from the same clone
share that Git config, so newly created worktrees use the hook automatically.
Fresh clones need the install step again.

The pre-commit hook runs:

```bash
tools/check_clang_format_changed.sh --staged --fix
```

It formats staged C/C++ files under `Source/` and `test/` with clang-format 18,
then re-stages the formatted files. The script uses a local clang-format 18 when
available and falls back to `ghcr.io/jidicula/clang-format:18` through Docker.

Run the same changed-file check manually before pushing:

```bash
tools/check_clang_format_changed.sh --base origin/master --head HEAD
```

The branch check compares from `merge-base(origin/master, HEAD)` to `HEAD`, so a
branch that is behind `origin/master` does not format-check C++ files that only
changed on `origin/master`.

## Native macOS arm64

Install dependencies, build the game, and run the preserved Hellfire fixture:

```bash
brew bundle install
cmake -S. -Bbuild-local-macos -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build-local-macos -j "$(sysctl -n hw.physicalcpu)" --target devilutionx

tools/run_render_timedemo_fixture.py \
  --binary build-local-macos/devilutionx.app/Contents/MacOS/devilutionx \
  --data-dir "$DATA_DIR" \
  --runs 3
```

The replay is valid only when every run reports:

```text
Timedemo: Same outcome as initial run. :)
```

The script prints the scratch directory and per-run log paths. Keep those logs
with benchmark notes when comparing render changes.

## Ubuntu arm64 with Docker

The recommended local Linux path uses Colima plus the Docker CLI:

```bash
brew install docker docker-buildx colima
mkdir -p ~/.docker/cli-plugins
ln -sf "$(brew --prefix docker-buildx)/bin/docker-buildx" ~/.docker/cli-plugins/docker-buildx
colima start --arch aarch64 --cpu 4 --memory 6 --disk 40
docker context use colima
```

Build and run inside an Ubuntu 24.04 arm64 container:

```bash
docker run --rm --platform linux/arm64 \
  --mount "type=bind,src=$PWD,dst=/src,readonly" \
  --mount "type=bind,src=$DATA_DIR,dst=/diablo-data,readonly" \
  ubuntu:24.04 bash -lc '
    set -euo pipefail
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y \
      cmake g++ git libbenchmark-dev libbz2-dev libfmt-dev libgmock-dev \
      libgtest-dev libpng-dev libsdl2-dev libsdl2-image-dev libsodium-dev \
      ninja-build pkg-config python3
    mkdir /work
    tar --exclude="./.git" --exclude="./build*" -C /src -cf - . | tar -C /work -xf -
    cd /work
    cmake -S. -Bbuild-local-linux-arm64 -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF \
      -DVERSION_SUFFIX=-local-docker
    cmake --build build-local-linux-arm64 --target devilutionx -j "$(nproc)"
    tools/run_render_timedemo_fixture.py \
      --binary build-local-linux-arm64/devilutionx \
      --data-dir /diablo-data \
      --scratch-dir /tmp/diablonext-render-timedemo \
      --runs 1
  '
```

This is useful for Linux correctness and repeatability checks. Do not compare
container FPS directly against native macOS FPS.

## Visible Ubuntu Demo with noVNC

The headless Docker runner does not open a window. To see the Ubuntu build run
the preserved Hellfire demo in a browser-accessible Linux desktop:

```bash
tools/run_ubuntu_visual_demo.sh
```

Open:

```text
http://localhost:6080/vnc.html?autoconnect=1&resize=scale
```

The noVNC desktop appears after the container installs Xvfb/noVNC. The
DiabloNext window appears after the in-container build finishes and the script
prints `VISUAL_STAGE: running demo loop`. The noVNC port is bound to
`127.0.0.1` only and does not require a password.

Watch progress:

```bash
tools/run_ubuntu_visual_demo.sh --logs
```

Stop the visual container:

```bash
tools/run_ubuntu_visual_demo.sh --stop
```

The visual demo intentionally omits `--timedemo` so playback is watchable. Use
`tools/run_render_timedemo_fixture.py` for same-outcome/FPS validation.

## Ubuntu amd64 Emulation Smoke Test

Colima can run amd64 containers through emulation:

```bash
docker run --rm --platform linux/amd64 ubuntu:24.04 uname -m
```

Expected output:

```text
x86_64
```

Use amd64 containers for build and replay smoke tests only. Timings are
emulation-tainted and should not be used as Linux desktop performance numbers.

## Current Local Coverage Matrix

| Target | Local status | Benchmark interpretation |
|---|---|---|
| macOS arm64 | Runnable benchmark | Best local baseline for this MacBook. |
| Ubuntu linux/arm64 Docker | Runnable benchmark | Linux correctness and rough regression signal. |
| Ubuntu linux/amd64 Docker | Optional smoke | Emulated; correctness only. |
| MinGW Windows | Deferred | Build likely possible in Linux container; Wine replay not set up. |
| iOS simulator | Deferred | Build may be possible; timedemo automation not defined. |
| macOS x86_64/Rosetta | Deferred | Dependency and runtime path needs separate setup. |
| BSDs, consoles, handhelds | Deferred | Classify build-only versus emulator-backed later. |

## Runner Notes

`tools/run_render_timedemo_fixture.py` prepares an isolated scratch save/config
area, copies `mods/Hellfire`, restores `single_0.hsv` before every run, and
fails if same-outcome validation is missing. Extra game arguments can be passed
after `--`:

```bash
tools/run_render_timedemo_fixture.py \
  --binary build-local-macos/devilutionx.app/Contents/MacOS/devilutionx \
  --data-dir "$DATA_DIR" \
  --runs 1 \
  -- --verbose
```
