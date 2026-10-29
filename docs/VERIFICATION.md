# Verification: is it really FSR 4?

## 0.2.0: FSR 4.1.2 (FSR SDK 2.4.0)

Run on an RX 6700 XT (RDNA 2), Mesa 26.2.5 RADV, BeamNG.drive 0.40.1.0 (step 9: 0.40.4.0), on 2026-10-29.
Every comparison is byte for byte.

1. **Which INT8 kernels AMD's DLL runs.** `amd_fidelityfx_upscaler_dx12.dll` from FSR SDK 2.4.0 (SHA-256
   `aad4fccc…`, provider `4.1.2`) has two INT8 providers. It chooses `ffxProvider_FSR4_Int8_Navi2` for GPU
   family NV with a Navi 2x revision. Otherwise it chooses the generic `ffxProvider_FSR4_Int8`, for RDNA 3/3.5.
   It reads the family through `D3DKMTQueryAdapterInfo`. Proton reports the RX 6700 XT there as RDNA 3, so under
   Proton the DLL runs the generic kernels. The capture harness can report the real Navi 22 family instead,
   and the DLL then runs its Navi2 kernels. Both sets were captured and translated as for 4.1.1.
2. **AMD's DLL ≡ the Vulkan port, for each set.** Same 7 cases as for 4.1.1 (both models, up to 3840×2160
   output, sizes not divisible by 8): **7/7 byte-identical for the Navi2 set, 7/7 for the generic set.** The
   reference DLL has one patched byte, the same buffer-UAV synchronization guard as the 4.1.1 reference. Without
   it the DLL's own output changes from run to run under vkd3d-proton. Shaders and weights are not modified.
3. **Same model.** Both sets use the same 128 KiB initializer as 4.1.1 and the same dispatch sequence. The Navi2
   set has its own kernels: a larger prepass, and pass11 tiled 8×4 instead of 64×1. The runtime now takes
   model-pass group counts from the shader's workgroup size.
4. **Image quality.** Ground-truth test: a detailed game frame, a camera that pans 5 or 11 px per frame
   (and a static one), 67% and 50% render scale, 1080p output, 48 frames. 4.1.1, 4.1.2 generic and 4.1.2
   Navi2 score within 0.01 dB PSNR of each other, with the same temporal error. 4.1.2 generic and 4.1.1 give
   identical scores.
5. **BeamNG loads the Navi2 set.** `events.log` reads `FSR4: GPU 1002:73df navi2x=1 variant=auto -> INT8 navi2`.
   All 29 programs a frame dispatches were created on BeamNG's device byte-identical to the files in
   `assets/navi2`. The two files not loaded are fallbacks this driver does not need.
6. **What BeamNG shows is that output.** Live frames after a reset (a runtime switch FSR 3 → FSR 4), 1715×965 →
   2560×1440, Sharpness 0.7, were replayed offline through the port with `assets/navi2`: **22/22 frames
   byte-identical**. The replay reads the weights from `initializer.bin`, so the live weights are those too.
7. **Long run and size changes.** 10,801 frames replaced, hard 0, jitter mismatches 0, no amdgpu errors. A monitor
   reconnect changed the game's output size twice during the run (1920×1080 → 2560×1440 → 1920×1080). Each time
   the layer showed FSR 3 for 17 frames while it relearned the jitter pattern, then resumed FSR 4 at the new
   size.
8. **Installed package.** `install.sh` from `beamng-fsr4-linux-0.2.0.tar.gz`, game started with only `BNG_FSR4=1`:
   the Navi2 set was picked, 1,801 frames replaced, hard 0, jitter mismatches 0. The menu reads "FSR 4.1.2 (Linux)"
   (`core_locales`, and the option list holds the swapped key).
9. **Steam launch, BeamNG.drive 0.40.4.0.** Steam updated the game from 0.40.1.0 to 0.40.4.0 before this launch.
   Launch options were `BNG_FSR4=1 %command%`. Steam ran the game in the Steam Linux Runtime (pressure-vessel,
   sniper). The game process had the installed layer, the Steam overlay and Fossilize loaded. The game's FSR 3
   shaders still matched the layer's fingerprints, and the layer picked the Navi2 set. In gridmap, with
   FSR 3 (custom) at 0.67 and 20 user mods active, 10,801 frames were replaced, hard 0, jitter mismatches 0,
   no amdgpu errors. The menu read "FSR 4.1.2 (Linux)".

## 0.1.0: FSR 4.1.1 (FidelityFX SDK 2.3.0)


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
