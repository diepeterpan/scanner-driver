#!/usr/bin/env bash
# Helper script to compile libsane-ricoh_sp110.so.1 using Docker
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="ricoh-sane-builder:latest"

echo "=== Building SANE backend with Docker ($IMAGE_NAME) ==="

# Build image if it doesn't exist
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    echo "Docker image not found, building $IMAGE_NAME..."
    docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"
fi

# Run make inside container with host UID/GID
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "$SCRIPT_DIR:/build" \
    "$IMAGE_NAME" clean all

echo "=== Compilation successful! ==="
ls -l "$SCRIPT_DIR"/libsane-ricoh_sp110*
