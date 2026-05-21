#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

CONTAINER_NAME=${CONTAINER_NAME:-diablonext-ubuntu-visual}
DATA_DIR=${DATA_DIR:-"$HOME/Library/Application Support/diasurgical/devilution"}
IMAGE=${IMAGE:-ubuntu:24.04}
PLATFORM=${PLATFORM:-linux/arm64}
NOVNC_PORT=${NOVNC_PORT:-6080}
VNC_GEOMETRY=${VNC_GEOMETRY:-1024x768x24}

usage() {
  cat <<EOF
Usage: $(basename "$0") [--stop] [--logs] [--help]

Starts a disposable Ubuntu container that builds DiabloNext, runs the Hellfire
demo in an Xvfb desktop, and exposes it through noVNC.

Environment overrides:
  DATA_DIR         Host MPQ data directory. Default: $DATA_DIR
  CONTAINER_NAME  Docker container name. Default: $CONTAINER_NAME
  IMAGE           Docker image. Default: $IMAGE
  PLATFORM        Docker platform. Default: $PLATFORM
  NOVNC_PORT      Host noVNC port. Default: $NOVNC_PORT
  VNC_GEOMETRY    Xvfb screen geometry. Default: $VNC_GEOMETRY

Open after start:
  http://localhost:$NOVNC_PORT/vnc.html?autoconnect=1&resize=scale
EOF
}

case "${1:-}" in
  --help|-h)
    usage
    exit 0
    ;;
  --stop)
    docker rm -f "$CONTAINER_NAME"
    exit 0
    ;;
  --logs)
    docker logs -f "$CONTAINER_NAME"
    exit 0
    ;;
  "")
    ;;
  *)
    echo "Unknown argument: $1" >&2
    usage >&2
    exit 64
    ;;
esac

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found on PATH" >&2
  exit 1
fi

if [[ ! -d "$DATA_DIR" ]]; then
  echo "DATA_DIR does not exist: $DATA_DIR" >&2
  exit 1
fi

docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true

CONTAINER_SCRIPT=$(cat <<'CONTAINER_SCRIPT'
set -euo pipefail

echo "VISUAL_STAGE: installing dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update >/tmp/visual-apt.log 2>&1
apt-get install -y \
  cmake g++ git libbenchmark-dev libbz2-dev libfmt-dev libgmock-dev \
  libgtest-dev libpng-dev libsdl2-dev libsdl2-image-dev libsodium-dev \
  ninja-build pkg-config python3 xvfb x11vnc fluxbox novnc websockify \
  >>/tmp/visual-apt.log 2>&1

echo "VISUAL_STAGE: starting vnc"
Xvfb :99 -screen 0 "${VNC_GEOMETRY:-1024x768x24}" -ac >/tmp/xvfb.log 2>&1 &
export DISPLAY=:99
fluxbox >/tmp/fluxbox.log 2>&1 &
x11vnc -display :99 -forever -shared -nopw -rfbport 5900 >/tmp/x11vnc.log 2>&1 &
websockify --web=/usr/share/novnc/ 0.0.0.0:6080 localhost:5900 >/tmp/novnc.log 2>&1 &

echo "VISUAL_STAGE: copying source"
mkdir /work
tar --exclude="./.git" --exclude="./build*" -C /src -cf - . | tar -C /work -xf -
cd /work

echo "VISUAL_STAGE: configuring"
cmake -S. -Bbuild-local-linux-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF \
  -DVERSION_SUFFIX=-local-visual \
  >/tmp/visual-cmake-configure.log 2>&1

echo "VISUAL_STAGE: building"
cmake --build build-local-linux-arm64 --target devilutionx -j "$(nproc)" \
  >/tmp/visual-build.log 2>&1

echo "VISUAL_STAGE: running demo loop"
export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER=x11

while true; do
  rm -rf /tmp/diablonext-visual-save /tmp/diablonext-visual-config
  mkdir -p /tmp/diablonext-visual-save/mods /tmp/diablonext-visual-config
  cp test/fixtures/timedemo/RenderTelemetryHellfireDemo1/demo_1.dmo /tmp/diablonext-visual-save/
  cp test/fixtures/timedemo/RenderTelemetryHellfireDemo1/single_0.hsv /tmp/diablonext-visual-save/
  cp test/fixtures/timedemo/RenderTelemetryHellfireDemo1/demo_1_reference_single_0.hsv /tmp/diablonext-visual-save/
  cp -a mods/Hellfire /tmp/diablonext-visual-save/mods/
  if [ -f /diablo-data/diablo.ini ]; then
    cp /diablo-data/diablo.ini /tmp/diablonext-visual-config/diablo.ini
  fi
  ./build-local-linux-arm64/devilutionx \
    --data-dir /diablo-data \
    --save-dir /tmp/diablonext-visual-save \
    --config-dir /tmp/diablonext-visual-config \
    --hellfire --demo 1 \
    --log-to-file /tmp/diablonext-visual-save/visual-demo.log \
    -n || true
  sleep 2
done
CONTAINER_SCRIPT
)

docker run -d \
  --name "$CONTAINER_NAME" \
  --platform "$PLATFORM" \
  -p "127.0.0.1:$NOVNC_PORT:6080" \
  --mount "type=bind,src=$REPO_ROOT,dst=/src,readonly" \
  --mount "type=bind,src=$DATA_DIR,dst=/diablo-data,readonly" \
  -e VNC_GEOMETRY="$VNC_GEOMETRY" \
  "$IMAGE" bash -lc "$CONTAINER_SCRIPT"

cat <<EOF
Started $CONTAINER_NAME.

Open:
  http://localhost:$NOVNC_PORT/vnc.html?autoconnect=1&resize=scale

The browser desktop appears after noVNC starts. The game window appears after
the in-container build reaches VISUAL_STAGE: running demo loop.

Watch progress:
  $0 --logs

Stop:
  $0 --stop
EOF
