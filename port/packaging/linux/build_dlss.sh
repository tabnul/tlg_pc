#!/usr/bin/env bash
# Builds libtlg_dlss.so (gpu/dlss_bridge, the only code that uses the NVIDIA DLSS SDK) and puts it
# with NVIDIA's libnvidia-ngx-dlss.so.<version> next to out/tlg-probe; package.sh ships both.
# Needs a checkout of https://github.com/NVIDIA/DLSS:
#   DLSS_SDK_ROOT=~/sdk/DLSS bash packaging/linux/build_dlss.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${DLSS_SDK_ROOT:?Set DLSS_SDK_ROOT to a checkout of github.com/NVIDIA/DLSS}
cmake -S gpu/dlss_bridge -B out/dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DDLSS_SDK_ROOT="$sdk" -DVULKAN_INCLUDE="$(pkg-config --variable=includedir vulkan)"
cmake --build out/dlss-bridge
cp out/dlss-bridge/libtlg_dlss.so out/
cp "$sdk"/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.* out/
cp "$sdk/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
echo "DLSS bridge ready: out/libtlg_dlss.so, $(cd out && echo libnvidia-ngx-dlss.so.*)"
