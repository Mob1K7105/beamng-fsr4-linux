#!/usr/bin/env bash
# Builds build/libVkLayer_bng_fsr4.so. Needs g++ (C++20) and libvulkan. Vulkan headers: VULKAN_HEADERS=<include dir>,
# else third_party/Vulkan-Headers (git submodule), else /usr/include. They must be new enough to define
# VK_VALVE_shader_mixed_float_dot_product (Vulkan-Headers 1.4.365 is pinned).
set -euo pipefail
cd "$(dirname "$0")"
inc=${VULKAN_HEADERS:-}
if [[ -z $inc ]]; then
    if [[ -f third_party/Vulkan-Headers/include/vulkan/vulkan_core.h ]]; then inc=third_party/Vulkan-Headers/include; else inc=/usr/include; fi
fi
mkdir -p build
# The FSR 4.1.1 runtime's own physical-device and buffer calls go through the layer's dispatch table.
redirect="-DvkCreateBuffer=f4_vkCreateBuffer -DvkCmdCopyBuffer=f4_vkCmdCopyBuffer -DvkGetPhysicalDeviceMemoryProperties=f4_vkGetPhysicalDeviceMemoryProperties -DvkGetPhysicalDeviceProperties=f4_vkGetPhysicalDeviceProperties"
g++ -std=c++20 -O2 -fPIC -c -I"$inc" -Ifsr411 $redirect fsr411/fsr411.cpp -o build/fsr411.o
g++ -std=c++20 -O2 -fPIC -c -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format -I"$inc" -Ifsr411 layer/bng_fsr4_layer.cpp -o build/bng_fsr4_layer.o
g++ -shared build/bng_fsr4_layer.o build/fsr411.o -o build/libVkLayer_bng_fsr4.so -lvulkan
echo "built $(pwd)/build/libVkLayer_bng_fsr4.so"
