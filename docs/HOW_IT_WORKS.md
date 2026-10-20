# How it works

## Where FSR 4 hooks in

BeamNG's `fsrCustom` upscaler records one block of compute passes per frame (debug label
`UpscaleDispatch`), all with descriptor set 0:

| Pass (`shaders/common/postFx/upscale/…`) | With the layer |
|---|---|
| `dynamicDisocclusion` (engine reactive mask) | runs |
| `fsr3/fsr3Clear` ×8 | skipped* |
| `upscalerExposure` (engine) | runs |
| `fsr3/fsr3PrepareInputs` | inputs captured, dispatch skipped* |
| `fsr3/fsr3LumaPyramid`, `ShadingChangePyramid`, `ShadingChange`, `PrepareReactivity`, `LumaInstability` | skipped* |
| `fsr3/fsr3Accumulate` | constant buffer copied for jitter, dispatch skipped* |
| `fsr3/fsr3Rcas` (final pass) | **replaced by FSR 4.1.1** writing the same output image |

\* Skipped only while FSR 4 is active; otherwise the original FSR 3 passes run. Nothing outside
`upscale/fsr3/` reads the FSR 3 intermediates, so skipping them does not affect the rest of the frame.

FSR 4 inputs, read from the bound descriptors:

- color: `r_input_color_jittered` (PrepareInputs binding 49), RGBA16F, render resolution
- depth: `r_input_depth` (48), D32S8 reversed-Z, depth aspect
- motion vectors: `r_input_motion_vectors` (47), RG16F in UV units → `motion_scale = render size`
- output: `rw_upscaled_output` (RCAS binding 31), RGBA16F, display resolution, layout `GENERAL`

Shaders are identified by the engine's debug names plus SPIR-V hashes. A game update that changes them
makes the layer fall back to FSR 3 automatically.

## Device setup

At `vkCreateDevice` the layer enables the features FSR 4 needs (shaderInt8, integer dot product,
`VK_VALVE_shader_mixed_float_dot_product` when available). If the driver rejects them, the original
device creation is retried and the layer stays passive.

## Jitter

FSR 4 needs the current frame's sub-pixel jitter while commands are recorded, but BeamNG writes it into a
GPU-side constant buffer. The layer copies that buffer back each frame (the copy arrives a few frames
late), learns the periodic jitter table (`jitterSeqLen` entries, Halton 2,3), and predicts the current
frame's value from a frame counter. Every prediction is later checked against the readback: 99.998% matched
over long runs. Resets (`frameIdx` back to 0), render-size and sequence-length changes invalidate the table.
The layer then falls back to FSR 3 until the table is relearned, typically 17–32 frames.

## Resets and switching

- **Game resets** (render-scale change, upscaler toggle, level load) are detected from the readback. If
  FSR 3 was skipped on that frame, FSR 4 renders it with a history reset instead (FSR 3 has no result to
  show), and the following frames run FSR 3 until the jitter is relearned.
- **Switching to FSR 3** (`fsr4.disable` file): FSR 4 stays on screen for 32 more frames while FSR 3 rebuilds
  its history, then hands over. Switching back starts FSR 4 with a history reset; it converges in about
  3 frames.

## Fallback

Every frame checks shader fingerprints, bindings, formats, sizes and the jitter state. Any unexpected
value leaves the game's FSR 3 output in place. Opt-in test hooks exercise these paths.
