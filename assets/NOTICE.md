Copyright (C) Advanced Micro Devices, Inc.

These files derive from AMD FSR SDK 2.4.0's
`Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll` (FSR 4.1.2),
SHA256 `aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace`.
AMD's complete notice and MIT exception for that DLL are retained in
[../notices/AMD-SDK-LICENSE.md](../notices/AMD-SDK-LICENSE.md).

Each set has 29 SPIR-V programs, an unmodified 128 KiB neural initializer and
an original postpass for comparison. They were captured from AMD's INT8 path,
translated from DXIL with dxil-spirv, and assigned Vulkan descriptor bindings.
Since 0.3.0 the postpass store pattern is rewritten by postpass_direct.py and the network
passes by model_clamp.py/model_tail.py (tools/exact-rewrites/, from fsr4-shader-performance-overrides,
GPL-2.0-or-later); the output is byte-identical to AMD's DLL. These are modified assets, not an
AMD-signed distribution.

`lite/` holds FSR 4.1.2 Lite (`ffxProvider_FSR4Lite_Int8`, which the DLL picks for Strix Point class
APUs; captured by reporting that GPU family to the DLL): 14 SPIR-V programs per set and its own
27 KiB initializer per model. Its postpass is AMD's (postpass.spv = postpass_orig.spv).

The DLL has two INT8 providers that run the same model. The sets in this folder are
the generic one (`ffxProvider_FSR4_Int8`, chosen for RDNA 3 and 3.5). `navi2/` holds
the RDNA 2 one (`ffxProvider_FSR4_Int8_Navi2`, chosen for Navi 2x GPUs): its own
kernels, with pass11 tiled 8x4 instead of 64x1. The layer loads `navi2/` on Navi 2x.

`t1080` covers output up to 1920x1080, `t2160` up to 3840x2160 and `t4320` larger output up to
7680x4320 (AMD's DLL picks them the same way and creates no context above 7680x4320). `m0` serves ratios through 2x; `m1` is the 3x Ultra Performance model.
The package's SHA256SUMS lists every file.
