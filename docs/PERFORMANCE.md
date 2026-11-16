# Performance

Measured on an RX 6700 XT (RDNA 2) with Mesa 26.2.4 RADV, BeamNG.drive 0.40 native Linux (Vulkan), at
2560×1440 output.

## Frozen-scene benchmark (West Coast USA)

The scene and camera stay the same in every mode: physics paused (`simTimeAuthority.pause(true)`), time of
day stopped, vsync off (`MESA_VK_WSI_PRESENT_MODE=immediate`), frame limiter and background limiter off.
Each mode ran 15 s in each of 2 rounds, and the rounds agreed within about 1 fps. FPS and GPU time are
BeamNG's own counters (`fps::fps`, `fps::gpuTime`).

| Mode | Render | FPS | GPU frame time |
|---|---|---:|---:|
| Native 100% (no upscaler, no AA) | 2560×1440 | 84 | 10.8 ms |
| FSR 3 @ 67% | 1715×965 | 102 | 8.6 ms |
| **FSR 4.1.1 @ 67%** | 1715×965 | **90** | **10.0 ms** |
| FSR 4.1.1 @ 67%, FSR 3 passes kept | 1715×965 | 81 | 11.2 ms |
| FSR 3 @ 50% | 1280×720 | 121 | 7.1 ms |
| **FSR 4.1.1 @ 50%** | 1280×720 | **103** | **8.6 ms** |

What the numbers show:
- At the same render scale, FSR 4.1.1 costs about 1.4 ms more GPU time than FSR 3 on RDNA 2.
- FSR 4.1.1 at 50% runs as fast as FSR 3 at 67%.
- FSR 4.1.1 at 67% is still faster than native 1440p, which also has no anti-aliasing.

Measured with the game's own frame counters (medians over a frozen scene).

## Upscaler block time (GPU timestamps)

The layer can time the whole upscale step on the GPU (`FSR4STUDY_TIME_CHAIN=1`). This was measured on
gridmap at 67%, as medians of 300-frame windows:

| Path | Upscale step |
|---|---:|
| BeamNG FSR 3 | 1.32 ms |
| FSR 4.1.1, FSR 3 passes kept | 3.87 ms |
| FSR 4.1.1, FSR 3 passes skipped (default) | 2.66 ms |

Skipping FSR 3's passes saves about 1.2 ms. Those passes have no effect on FSR 4's output: replays matched
byte for byte. On its own, FSR 4.1.1 costs about 2.68 ms per frame (median of 128 × 300 frames). Measuring
the 50% scale this way gave noisy results, so the table has no 50% row.

## Speed work (2026-10-20)

**CPU: the layer's own overhead.** By default the layer now runs lean. It intercepts only the 12 Vulkan calls
FSR 4 needs, and the game's draws, copies, barriers and submits go straight to the driver. Same day, same
frozen scene, BeamNG's `fps::cpuTime`:

| Mode | full diagnostic layer | lean (default) |
|---|---:|---:|
| Native 100% | 6.20 ms | 4.98 ms |
| FSR 3 @ 67% | 6.21 ms | 5.07 ms |
| FSR 4.1.1 @ 67% | 6.51 ms | 5.11 ms |
| FSR 4.1.1 @ 50% | 6.54 ms | 4.99 ms |

That is 1.2–1.5 ms less CPU time per frame. The frozen West Coast scene is GPU-bound, so its FPS does not change.
CPU-bound scenes (many vehicles, AI traffic, gridmap) are where it shows. The log also shrank from about 4.8 MB to
12 KB per session.

**GPU: FSR 4's own passes.** Offline, 1707×960 → 2560×1440, 300-frame medians:

| Change | FSR 4.1.1 GPU time | Output |
|---|---:|---|
| before | 2.674 ms | AMD-DLL reference |
| per-pass wave size (wave32 for the image passes, wave64 for the network) | 2.640 ms | byte-identical, 7/7 parity cases |

The skip set now also includes the engine's FSR 3 reactive-mask pass (`dynamicDisocclusion`). These GPU savings
are smaller than the run-to-run noise of whole-frame FPS, so they are measured offline. Checked and not
worth taking:
- The INT8 network already compiles to hardware `v_dot4` instructions and does not spill.
- Removing barriers is worth 0.15 ms at most, and only 0.03 ms of that can be removed without proving there is no data race.

## FSR 4.1.2 (2026-10-29)

0.2.0 uses AMD's RDNA 2 INT8 kernels (the 4.1.2 DLL's Navi2 provider) on Navi 2x. Offline GPU time on an
RX 6700 XT, medians over 600 frames, three rounds that agreed within 0.02 ms:

| Render → output | FSR 4.1.1 | 4.1.2 generic | **4.1.2 Navi2** |
|---|---:|---:|---:|
| 1280×720 → 1920×1080 | 1.527 ms | 1.525 ms | **1.482 ms** |
| 1280×720 → 2560×1440 (50%) | 2.635 ms | 2.633 ms | **2.560 ms** |
| 1707×960 → 2560×1440 (67%) | 2.648 ms | 2.642 ms | **2.589 ms** |
| 2259×1271 → 3840×2160 | 5.902 ms | 5.897 ms | **5.818 ms** |

At 1440p that is about 0.06–0.07 ms less per frame (−2 to −3%). Most of it comes from the prepass (−0.05 ms)
and pass11 (−0.02 ms). In a GPU-bound scene at about 90 fps, this is worth about half a frame per second. The
in-game tables above were measured with 4.1.1.

## 0.3.0: exact shader rewrites and FSR 4.1.2 Lite (2026-11-06)

Offline GPU time on an RX 6700 XT (Mesa RADV), medians over 600 frames, two or three rounds that agreed within
0.04 ms. "Exact" means the output is byte-identical to 0.2.1 and to AMD's DLL.

| Render → output | 0.2.1 (Navi2) | **0.3.0 (Navi2, exact)** | **0.3.0 Lite** (`BNG_FSR4_VARIANT=lite`) |
|---|---:|---:|---:|
| 1280×720 → 1920×1080 | 1.472 ms | **1.359 ms** (−8%) | |
| 1707×960 → 2560×1440 (67%) | 2.586 ms | **2.403 ms** (−7%) | **1.54 ms** (−36% against 0.3.0) |
| 2259×1271 → 3840×2160 | 5.796 ms | **5.36 ms** (−7.5%) | |
| 2970×835 → 5120×1440 (58%) | 5.118 ms | **4.754 ms** (−7%) | **3.18 ms** (−33% against 0.3.0) |

- Nearly all of the exact gain is the postpass: `postpass_direct.py` stages one image at a time through 16 KB of
  workgroup memory (0.2.1's rewrite used 40 KB for all three), so more waves fit on RDNA 2. The integer rounding and
  clamping rewrites of the network passes add about 0.02 ms. The generic set gets the same rewrites.
- Lite keeps AMD's postpass: both postpass rewrites were slower on it (3.25 and 3.47 ms at 5120×1440). Its
  per-frame clear of 33 MB or less costs a few microseconds.
- Not applied: the overrides project's prepass rewrite is for FSR 4.1.1's prepass (4.1.2's is a different shader),
  its pass11 change is already in AMD's Navi2 kernels, and its shared-taps postpass variant does not match this
  postpass. Their lossy builds (frame skipping, retrained weights) change the picture and are not used.

### 0.3.0 in-game (2026-11-16)

The installed 0.3.0 package (launched with only `BNG_FSR4=1`), RX 6700 XT, Mesa 26.2.6, BeamNG 0.40.4.0, 2560×1440
output, vsync off. Frozen West Coast scene as above: 15 s per mode, 3 rounds that agreed within about 1.5 fps, medians.

| Mode | FPS | GPU frame time |
|---|---:|---:|
| Native 100% | 75.6 | 11.8 ms |
| FSR 3 @ 58% | 106.3 | 8.0 ms |
| **FSR 4 @ 58%** | **95.4** | **9.1 ms** |
| **FSR 4 Lite @ 58%** (`BNG_FSR4_VARIANT=lite`) | **103.1** | **8.3 ms** |
| FSR 3 @ 67% | 96.7 | 9.0 ms |
| FSR 4 @ 67% | 87.5 | 10.0 ms |

Upscale step on the GPU (`FSR4STUDY_TIME_CHAIN=1`, gridmap at 58%, steady 300-frame medians): FSR 4 2.30 ms,
the generic INT8 set 2.33 ms, Lite 1.48 ms. In the first ~1000 frames after launch some windows read about
10 ms; they settle once the GPU is under steady load.

Driving with AI traffic (7 cars, the player car on AI), FSR 4 and FSR 3 alternating in 25 s windows, BeamNG's
per-second counters at 58%: FSR 4 93–97 fps (GPU 8.5–8.9 ms), Lite 97–104 fps (GPU 7.7–8.1 ms), FSR 3 98–103 fps
(GPU 7.7–8.1 ms). No sample went over 16.7 ms of GPU time, so with vsync at 60 Hz all three hold 60 fps on
this GPU in these scenes.

## High output resolutions (5120×1440 and up)

FSR 4's cost follows the **output** size. 5120×1440 has twice the pixels of 2560×1440, so FSR 4 costs about
twice as much. Offline on the RX 6700 XT, 3328×936 → 5120×1440 (65%): 4.78 ms (Navi2), 4.90 ms (generic),
3.25 ms (Lite). At that output size FSR 4 can use up most of what the lower render scale saves. One user report
(RX 9060 XT, generic set, 5120×1440 at 65%) measured 16.3 ms GPU time with FSR 4 against 9.4 ms with SMAA at the
same scale.

What helps:
- Try Lite (`BNG_FSR4_VARIANT=lite BNG_FSR4=1 %command%`): about a third cheaper.
- Compare against FSR 3 (custom) at the same scale, not against SMAA/FXAA, which cost almost nothing.
- BeamNG's "Off" anti-aliasing mode ignores the Resolution Scaling slider and renders at full resolution.

RDNA 4 has FP8 hardware that AMD's own FSR 4 uses on Windows. This layer only has the INT8 kernels, so RDNA 3 and
RDNA 4 run the generic INT8 set.

## Why FSR 4 costs more on RDNA 2

FSR 4.1.1 is a neural network. RDNA 2 has no WMMA or FP8 hardware, so the INT8 model runs on packed dot
products (`VK_VALVE_shader_mixed_float_dot_product`). Its cost mostly scales with output resolution, not
render resolution. A lower render scale therefore recovers frames without making FSR 4's own pass cheaper.

## Measuring on your system

1. Enable BeamNG's FPS display, or use MangoHud.
2. Compare the same spot with the same camera, with physics paused (`J`).
3. Toggle FSR 4 at runtime with `touch <output dir>/fsr4.disable` (FSR 3 takes over within 32 frames), then
   delete the file to switch back.
4. In bug reports, include GPU, Mesa version, render scale, output resolution and the
   `FSR4: frames replaced … fail-open …` log line.
