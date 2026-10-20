This is an unofficial FSR 4.1.1 INT8 port for native Linux/Vulkan. It is not
an AMD release and is not affiliated with or endorsed by AMD. AMD and FidelityFX
names and trademarks belong to their respective owners.

The Vulkan runtime, benchmark and capture/extraction tools are adapted from
[deadinside28's bbport](https://github.com/deadinside28/bloodborne_pc), revision
`a219d841546b2ca28a3fefb11268ce7d5ca54192`, under GPL-2.0-or-later.
The original SPDX notices remain in the source. Local changes isolate the
benchmark, add feature checks and output verification, and package the build.
See LICENSE for the complete GPL text. This runtime is not an MIT-licensed SDK
for unrestricted inclusion in a proprietary game.

The shader programs and neural weights in assets/ derive from AMD's
`amd_fidelityfx_upscaler_dx12.dll` in FidelityFX SDK 2.3.0, revision
`60f4ea81909200d8542eca14dccb2628b763a9a3`. AMD's SDK notice explicitly lists
that DLL under its MIT exception. The full notice, including the file list and
exception, is retained in notices/AMD-SDK-LICENSE.md. This does not relicense
other parts of the SDK. See assets/NOTICE.md for the transformations applied.
No AMD DLL is included in this repository.

The reference DLL preparation uses the hardware-eligibility and buffer-UAV
synchronization patches documented by
[daniel-h-0/bc250-fsr4-fork](https://github.com/daniel-h-0/bc250-fsr4-fork),
revision `528f13b17e48bfba5b153f17ec4ebdfb3afa5bcb`. Its new tools use the
MIT grant retained in notices/BC250-MIT. The reference DLL keeps AMD's original
shaders and model weights. It is modified and must not be described as AMD-signed.

Shader translation uses Hans-Kristian Arntzen's dxil-spirv for Valve, under MIT;
see notices/dxil-spirv-LICENSE.MIT. Vulkan Headers and Validation Layers are
Khronos projects with their own notices in the fetched source. Wine, Proton,
DXVK and vkd3d-proton are optional asset-preparation/reference dependencies;
their upstream licenses apply. They are not linked into the native runtime.

This software is experimental and supplied without warranty. Validation on
one GPU and synthetic inputs is not a game-quality, cross-vendor, HDR or
platform-support guarantee. The included model is INT8; it is not the RDNA 4
FP8 model. No frame generation or ray reconstruction is provided.
