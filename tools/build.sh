#!/usr/bin/env bash
#
# Build the camera firmware.
#
# Everything happens inside Beken's official Docker image, so the toolchain is
# byte-identical on Linux, macOS and Windows (WSL2). Nothing is installed on
# your machine except Docker itself.
#
#     ./tools/build.sh            build
#     ./tools/build.sh clean      remove build output
#     ./tools/build.sh <args>     anything else is passed straight to make
#
# Runs from any directory.

set -euo pipefail

IMAGE="bekencorp/armino-idk:1.2"
SOC="bk7258"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ── Preflight ────────────────────────────────────────────────────────────────

if ! command -v docker >/dev/null 2>&1; then
    cat <<'EOF'
Docker is not installed, or is not on your PATH.

  Linux    https://docs.docker.com/engine/install/
  macOS    https://docs.docker.com/desktop/install/mac-install/
  Windows  https://docs.docker.com/desktop/install/windows-install/  (use WSL2)
EOF
    exit 1
fi

if ! docker info >/dev/null 2>&1; then
    cat <<'EOF'
Cannot reach the Docker daemon.

Either it is not running, or your user is not in the "docker" group. On Linux:

    sudo usermod -aG docker $USER      # then log out and back in

Do NOT work around this with sudo. The build runs the container as your own
user; under sudo every generated file becomes root-owned, and the next normal
build fails with a permission error that needs "sudo rm -rf sdk/build" to fix.
EOF
    exit 1
fi

if [ ! -f sdk/Makefile ] || [ ! -f libpeer/CMakeLists.txt ]; then
    cat <<'EOF'
The submodules are missing.

The Beken SDK and libpeer are separate repositories. Fetch them with:

    git submodule update --init --recursive

(Next time, clone with --recursive.)
EOF
    exit 1
fi

# ── Build ────────────────────────────────────────────────────────────────────
#
# The application and libpeer live in THIS repository, not inside the SDK, so
# the SDK stays a pristine checkout of Beken's own tree — you can diff it
# against upstream and see exactly the small patch set we carry.
#
# Two environment variables make that work:
#
#   PROJECT_DIR            where the application lives   (sdk/tools/.../build_main.mk)
#   EXTRA_COMPONENTS_DIRS  extra component search paths  (sdk/tools/.../project.cmake)
#
# make itself must run from the SDK root, because the build derives ARMINO_DIR
# from the current working directory.

ARGS=("$@")
if [ ${#ARGS[@]} -eq 0 ]; then
    ARGS=(make "$SOC" "PROJECT=anedya-camera-livestream")
fi

DOCKER_OPTS=(--rm -v "$REPO_ROOT:/armino" -w /armino/sdk -u "$(id -u)")
DOCKER_OPTS+=(-e PROJECT_DIR=/armino/app)
DOCKER_OPTS+=(-e EXTRA_COMPONENTS_DIRS=/armino/libpeer)

# The image is amd64-only; Apple Silicon and other non-x86 hosts need emulation.
if [ "$(uname -m)" != "x86_64" ]; then
    DOCKER_OPTS+=(--platform linux/amd64)
fi

echo "Building for ${SOC} ..."
docker run "${DOCKER_OPTS[@]}" "$IMAGE" "${ARGS[@]}"

FIRMWARE=$(find "$REPO_ROOT/sdk/build" -name all-app.bin 2>/dev/null | head -1)
if [ -n "$FIRMWARE" ]; then
    echo
    echo "Firmware: ${FIRMWARE#"$REPO_ROOT"/}"
    echo "See the README for how to flash it."
fi
