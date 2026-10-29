Copyright (C) Advanced Micro Devices, Inc.

These files derive from AMD FSR SDK 2.4.0's
`Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll` (FSR 4.1.2),
SHA256 `aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace`.
AMD's complete notice and MIT exception for that DLL are retained in
[../notices/AMD-SDK-LICENSE.md](../notices/AMD-SDK-LICENSE.md).

Each set has 29 SPIR-V programs, an unmodified 128 KiB neural initializer and
an original postpass for comparison. They were captured from AMD's INT8 path,
translated from DXIL with dxil-spirv, and assigned Vulkan descriptor bindings.
The postpass store pattern is rewritten by bbport's postpass_lds.py; its original
arithmetic is retained. These are modified assets, not an AMD-signed distribution.

The DLL has two INT8 providers that run the same model. The sets in this folder are
the generic one (`ffxProvider_FSR4_Int8`, chosen for RDNA 3 and 3.5). `navi2/` holds
the RDNA 2 one (`ffxProvider_FSR4_Int8_Navi2`, chosen for Navi 2x GPUs): its own
kernels, with pass11 tiled 8x4 instead of 64x1. The layer loads `navi2/` on Navi 2x.

`t1080` covers output up to 1920x1080; `t2160` covers larger output up to
3840x2160. `m0` serves ratios through 2x; `m1` is the 3x Ultra Performance model.
The package's SHA256SUMS lists every file.
