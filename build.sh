#!/bin/bash

# DetectX Client Build Script
#
# Usage:
#   ./build.sh                    # Build for aarch64 with cache
#   ./build.sh --clean            # Build for aarch64 without cache
#   ./build.sh --arch armv7hf     # Build for armv7hf
#   ./build.sh --arch aarch64     # Build for aarch64
#   ./build.sh --all              # Build for both architectures
#
# Options:
#   --clean         Force rebuild without cache
#   --arch <arch>   Build for specific architecture (aarch64 or armv7hf)
#   --all           Build for all supported architectures

set -e  # Exit on error

CACHE_FLAG=""
CHIP="aarch64"  # Default
BUILD_ALL=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            CACHE_FLAG="--no-cache"
            echo "Clean build enabled (no cache)"
            shift
            ;;
        --arch)
            CHIP="$2"
            shift 2
            ;;
        --all)
            BUILD_ALL=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--clean] [--arch aarch64|armv7hf] [--all]"
            exit 1
            ;;
    esac
done

# Function to build for a specific architecture
build_for_arch() {
    local arch=$1
    echo ""
    echo "==========================================="
    echo "Building for $arch"
    echo "==========================================="

    docker build --progress=plain $CACHE_FLAG --build-arg CHIP=$arch . -t detectx-client-$arch

    echo ""
    echo "=== Extracting .eap file from container ==="
    CONTAINER_ID=$(docker create detectx-client-$arch)
    docker cp $CONTAINER_ID:/opt/app ./build-$arch
    docker rm $CONTAINER_ID

    echo ""
    echo "=== Copying .eap file to current directory ==="
    cp -v ./build-$arch/*.eap .

    echo ""
    echo "=== Cleaning up build directory ==="
    rm -rf ./build-$arch

    echo ""
    echo "=== Build complete for $arch ==="
    ls -lh *_${arch}.eap
}

# Build based on options
if [ "$BUILD_ALL" = true ]; then
    echo "Building for all supported architectures..."
    build_for_arch "aarch64"
    build_for_arch "armv7hf"
    echo ""
    echo "==========================================="
    echo "All builds complete!"
    echo "==========================================="
    ls -lh *.eap
else
    # Validate architecture
    if [[ "$CHIP" != "aarch64" && "$CHIP" != "armv7hf" ]]; then
        echo "Error: Invalid architecture '$CHIP'"
        echo "Supported: aarch64, armv7hf"
        exit 1
    fi

    build_for_arch "$CHIP"
fi