#!/bin/bash

# DetectX Client Build Script
#
# Usage:
#   ./build.sh                    # Build for both architectures (aarch64 and armv7hf) with cache
#   ./build.sh --clean            # Build for both architectures without cache
#   ./build.sh --arch armv7hf     # Build only for armv7hf
#   ./build.sh --arch aarch64     # Build only for aarch64
#
# Options:
#   --clean         Force rebuild without cache
#   --arch <arch>   Build for specific architecture only (aarch64 or armv7hf)

set -e  # Exit on error

CACHE_FLAG=""
CHIP=""  # Empty means build all
BUILD_ALL=true  # Default to building all architectures

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
            BUILD_ALL=false  # Override default when specific arch requested
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--clean] [--arch aarch64|armv7hf]"
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
    echo ""
    echo "==========================================="
    echo "Building for all supported architectures"
    echo "  - aarch64 (ARTPEC-8/9)"
    echo "  - armv7hf (ARTPEC-7)"
    echo "==========================================="
    echo ""
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

    echo "Building for $CHIP only..."
    build_for_arch "$CHIP"
fi