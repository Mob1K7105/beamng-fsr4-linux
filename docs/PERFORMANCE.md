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
