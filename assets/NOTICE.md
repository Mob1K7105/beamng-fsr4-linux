Copyright (C) Advanced Micro Devices, Inc.

These files derive from AMD FidelityFX SDK 2.3.0's
`Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll` (FSR 4.1.1),
SHA256 `d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46`.
AMD's complete notice and MIT exception for that DLL are retained in
[../notices/AMD-SDK-LICENSE.md](../notices/AMD-SDK-LICENSE.md).

Each set has 29 SPIR-V programs, an unmodified 128 KiB neural initializer and
an original postpass for comparison. They were captured from AMD's INT8 path,
translated from DXIL with dxil-spirv, and assigned Vulkan descriptor bindings.
The postpass store pattern is rewritten by bbport's postpass_lds.py; its original
arithmetic is retained. These are modified assets, not an AMD-signed distribution.

`t1080` covers output up to 1920x1080; `t2160` covers larger output up to
3840x2160. `m0` serves ratios through 2x; `m1` is the 3x Ultra Performance model.
All files have pinned SHA256 hashes in ../dependencies.json.
