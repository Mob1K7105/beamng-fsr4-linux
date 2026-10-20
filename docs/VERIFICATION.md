# Verification: is it really FSR 4.1.1?

Each step below is an exact, byte-for-byte comparison, not a similarity score. It was run on an RX 6700 XT
(RDNA 2), Mesa 26.2.4 RADV, BeamNG.drive 0.40.0.0, on 2026-10-19.

1. **AMD's DLL ≡ the Vulkan port.** AMD's signed `amd_fidelityfx_upscaler_dx12.dll` from FidelityFX SDK
   2.3.0 (SHA-256 `d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46`, provider
   `4.1.1`) was run under Wine/vkd3d-proton and compared with the native port (`tools/verify.py` of
   FSR-4.1.1-linux). All 7 cases matched byte for byte, both models, up to 3840×2160 output.
2. **BeamNG loads exactly those assets.** All 29 SPIR-V programs created on BeamNG's device are
   byte-identical to the pinned asset files. The 131,072-byte model initializer read back from GPU
   memory equals `initializer.bin` (0 differing bytes).
3. **What BeamNG shows is that output.** Live frames were captured with their color, depth, motion and
   jitter. Replaying them offline through the port reproduced every live frame byte for byte: 33/33 frames,
   and 53/53 with the FSR 3 passes skipped.

Additional checks:

- **Vulkan validation** (Khronos layer, synchronization validation, message limit off): the same message
  types as the unmodified game. None involves the layer's commands.
- **Stability:** more than 2 hours of sessions, over 150,000 FSR 4 frames, level changes, render-scale and
  mode switches. No GPU resets, crashes or backend failures.
- **Jitter prediction:** 99.998% (one miss per same-size game reset).

The evidence logs and test scripts are kept in the author's development tree and are not part of this repository.
