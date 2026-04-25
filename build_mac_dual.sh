#!/bin/bash
set -e

VCPKG_TOOLCHAIN="${VCPKG_ROOT:-$HOME/develop/vcpkg}/scripts/buildsystems/vcpkg.cmake"

echo "==========================================="
echo " Building x86_64 (Rosetta) Binaries..."
echo "==========================================="
rm -rf build_x86_64
cmake -B build_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN" \
    -DVCPKG_TARGET_TRIPLET=x64-osx-release \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=_private/vstsdk2.4
cmake --build build_x86_64 --target rps-pluginscanner rps-pluginhost

echo "==========================================="
echo " Building arm64 (Native) Binaries..."
echo "==========================================="
rm -rf build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN" \
    -DVCPKG_TARGET_TRIPLET=arm64-osx-release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DRPS_ENABLE_VST2=ON -DRPS_VST2_SDK_PATH=_private/vstsdk2.4
cmake --build build

echo "==========================================="
echo " Bundling Binaries..."
echo "==========================================="
for f in $(find build -name "rps-pluginscanner" -type f); do
    cp build_x86_64/apps/rps-pluginscanner/rps-pluginscanner "$(dirname "$f")/rps-pluginscanner_x86_64"
done
for f in $(find build -name "rps-pluginhost" -type f); do
    cp build_x86_64/apps/rps-pluginhost/rps-pluginhost "$(dirname "$f")/rps-pluginhost_x86_64"
done

echo "Dual-architecture build complete!"
